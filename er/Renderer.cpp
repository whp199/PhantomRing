#include "Renderer.h"

#include "Config.h"
#include "D3D12Hooks.h"
#include "D3D12Util.h"
#include "Gbuffer.h"
#include "Host.h"
#include "Link.h"
#include "Log.h"
#include "Overlay.h"
#include "PacingProbe.h"
#include "RenderAssets.h"
#include "RenderDepth.h"
#include "RenderProbe.h"
#include "RenderPso.h"
#include "RenderMarkers.h"
#include "RenderScene.h"
#include "RenderBind.h"
#include "RenderShared.h"
#include "RenderView.h"
#include "Shaders.h"
#include "Tasks.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <numbers>
#include <utility>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace phantom::er::Renderer
{
	namespace
	{
		using d3d::ComPtr;

		using render::FrameConstants;
		using render::UploadCursor;

		// One context per submission of ours (up to three per frame: stage C G-buffer, stage C velocity,
		// stage A/B), each with its own allocator, upload slice and descriptor slice, reused once its
		// fence passed.
		constexpr UINT          kFrames = 8;
		constexpr std::uint64_t kUploadPerFrame = 8ull << 20;   // frame constants + bone matrices (x2 with the previous frame's)
		constexpr std::uint64_t kUploadSlack = 64ull << 10;     // a shader may read past the last draw's bones (cbuffer size)
		constexpr UINT          kDescPerFrame = 6 * 2048;
		constexpr UINT          kRtvSlots = 16;

		struct FrameCtx
		{
			ComPtr<ID3D12CommandAllocator>     alloc;
			std::uint64_t                      fence{ 0 };
			std::vector<std::shared_ptr<void>> keep;
		};

		std::recursive_mutex g_lock;
		std::atomic<bool>    g_off{ false };   // gave up (logged)
		bool                 g_ready{ false };

		ComPtr<ID3D12Device>              g_device;
		ComPtr<ID3D12CommandQueue>        g_queue;
		IDXGISwapChain*                   g_swap{ nullptr };
		ComPtr<ID3D12GraphicsCommandList> g_list;
		ComPtr<ID3D12Fence>               g_fence;
		HANDLE                            g_event{ nullptr };
		std::uint64_t                     g_fenceValue{ 0 };
		FrameCtx                          g_ctx[kFrames];
		UINT                              g_ctxIndex{ 0 };

		ComPtr<ID3D12Resource>       g_upload;
		std::uint8_t*                g_uploadPtr{ nullptr };
		D3D12_GPU_VIRTUAL_ADDRESS    g_uploadVa{ 0 };
		ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_dsvHeap, g_gpuHeap, g_nullHeap;
		UINT                         g_rtvInc{ 0 }, g_srvInc{ 0 };
		ComPtr<ID3D12RootSignature>  g_root;

		std::vector<ComPtr<ID3D12Resource>> g_backBuffers;
		DXGI_FORMAT                         g_bbFormat{ DXGI_FORMAT_UNKNOWN };
		DXGI_FORMAT                         g_rtFormat{ DXGI_FORMAT_UNKNOWN };  // of the target being drawn into now
		UINT                                g_width{ 0 }, g_height{ 0 };
		ComPtr<ID3D12Resource>              g_depth;
		float                               g_outputMode{ 0 };

		std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> g_psos;
		std::unordered_set<std::uint64_t>                             g_badPsos;

		// stats
		std::atomic<std::uint64_t> g_depthFrames{ 0 }, g_sceneFrames{ 0 }, g_gbufFrames{ 0 }, g_fallbackFrames{ 0 };
		bool                       g_sceneDrawn{ false };  // stage B (or C) drew this frame: Present doesn't
		bool                       g_markersDrawn{ false };  // er/RenderMarkers.h drawn before the HUD this frame
		std::unordered_map<int, ComPtr<ID3D12PipelineState>> g_markerPsos;  // by render-target format
		bool                       g_markerFailed{ false };
		std::atomic<std::uint64_t> g_markerFrames{ 0 }, g_markerPresentFrames{ 0 };
		bool                       g_gbufDone{ false };    // stage C's G-buffer point handled this frame
		bool                       g_velDone{ false };     // stage C's velocity point handled this frame
		bool                       g_gbufDrawn{ false };   // Snake is in this frame's G-buffer
		std::string                g_stageWhy;             // why stage C is not drawing (logged on change)
		ID3D12Resource*            g_sceneTarget{ nullptr };
		DXGI_FORMAT                g_sceneRtvFormat{ DXGI_FORMAT_UNKNOWN };
		std::atomic<std::uint64_t> g_presents{ 0 }, g_framesDrawn{ 0 }, g_drawsIssued{ 0 }, g_drawsMissing{ 0 }, g_drawsBad{ 0 }, g_skipped{ 0 };
		double                     g_cpuMsAvg{ 0 };
		std::uint64_t              g_lastStatLog{ 0 };
		DWORD                      g_presentThread{ 0 };

		void TurnOff(std::string_view a_why)
		{
			if (!g_off.exchange(true)) {
				Log::Error("render: {}; renderer off for this session (Elden Ring's frame is untouched)", a_why);
				Link::Get().SetRenderConsumer(false);
			}
		}

		bool Check(HRESULT a_hr, std::string_view a_what)
		{
			if (FAILED(a_hr)) {
				TurnOff(std::format("{} failed: {}", a_what, d3d::Hr(a_hr)));
				return false;
			}
			return true;
		}

		void WaitIdle()
		{
			if (!g_queue || !g_fence) {
				return;
			}
			const auto v = ++g_fenceValue;
			if (FAILED(g_queue->Signal(g_fence.Get(), v))) {
				return;
			}
			d3d::WaitFence(g_fence.Get(), g_event, v, 2000);
		}

		bool CreateRootSignature()
		{
			HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
			auto    serialize = d3d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12SerializeRootSignature"))) : nullptr;
			if (!serialize) {
				TurnOff("D3D12SerializeRootSignature missing");
				return false;
			}
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = proto::kDrawTextures;
			range.BaseShaderRegister = 0;
			range.OffsetInDescriptorsFromTableStart = 0;

			D3D12_DESCRIPTOR_RANGE depthRange{};
			depthRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			depthRange.NumDescriptors = 1;
			depthRange.BaseShaderRegister = 6;

			D3D12_ROOT_PARAMETER params[7]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[0].Descriptor.ShaderRegister = 0;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[1].Constants.ShaderRegister = 1;
			params[1].Constants.Num32BitValues = render::kDrawConstants;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[2].Descriptor.ShaderRegister = 2;
			params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[3].DescriptorTable.NumDescriptorRanges = 1;
			params[3].DescriptorTable.pDescriptorRanges = &range;
			params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[4].DescriptorTable.NumDescriptorRanges = 1;
			params[4].DescriptorTable.pDescriptorRanges = &depthRange;
			params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[5].Descriptor.ShaderRegister = 3;
			params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
			params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			params[6].Descriptor.ShaderRegister = 4;
			params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_ANISOTROPIC;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.MaxAnisotropy = 8;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = 0;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

			D3D12_ROOT_SIGNATURE_DESC desc{};
			desc.NumParameters = 7;
			desc.pParameters = params;
			desc.NumStaticSamplers = 1;
			desc.pStaticSamplers = &sampler;
			desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			ComPtr<ID3DBlob> blob, error;
			if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf()))) {
				TurnOff(std::format("root signature: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "?"));
				return false;
			}
			return Check(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
							 reinterpret_cast<void**>(g_root.GetAddressOf())),
				"CreateRootSignature");
		}

		bool Init(IDXGISwapChain* a_swap)
		{
			g_queue = D3D12Hooks::PresentQueue(a_swap);
			if (!g_queue) {
				return false;  // not known yet: the hooks keep looking (ExecuteCommandLists)
			}
			if (!Check(g_queue->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(g_device.GetAddressOf())), "queue GetDevice")) {
				return false;
			}
			for (auto& c : g_ctx) {
				if (!Check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(c.alloc.GetAddressOf())),
						"CreateCommandAllocator")) {
					return false;
				}
			}
			if (!Check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ctx[0].alloc.Get(), nullptr, __uuidof(ID3D12GraphicsCommandList),
						   reinterpret_cast<void**>(g_list.GetAddressOf())),
					"CreateCommandList")) {
				return false;
			}
			g_list->Close();
			if (!Check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(g_fence.GetAddressOf())), "CreateFence")) {
				return false;
			}
			g_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

			const auto props = d3d::HeapProps(D3D12_HEAP_TYPE_UPLOAD);
			const auto ud = d3d::BufferDesc(kFrames * kUploadPerFrame + kUploadSlack);
			if (!Check(g_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &ud, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
						   reinterpret_cast<void**>(g_upload.GetAddressOf())),
					"upload ring")) {
				return false;
			}
			if (!Check(g_upload->Map(0, nullptr, reinterpret_cast<void**>(&g_uploadPtr)), "upload Map")) {
				return false;
			}
			std::memset(g_uploadPtr, 0, ud.Width);
			g_uploadVa = g_upload->GetGPUVirtualAddress();

			auto heap = [](D3D12_DESCRIPTOR_HEAP_TYPE a_type, UINT a_n, bool a_visible, ComPtr<ID3D12DescriptorHeap>& a_out) {
				D3D12_DESCRIPTOR_HEAP_DESC d{};
				d.Type = a_type;
				d.NumDescriptors = a_n;
				d.Flags = a_visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
				return Check(g_device->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(a_out.GetAddressOf())), "CreateDescriptorHeap");
			};
			if (!heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots, false, g_rtvHeap) || !heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, false, g_dsvHeap) ||
				!heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kFrames * kDescPerFrame, true, g_gpuHeap) ||
				!heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, false, g_nullHeap)) {
				return false;
			}
			g_rtvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
			g_srvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
			nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			nullSrv.Texture2D.MipLevels = 1;
			g_device->CreateShaderResourceView(nullptr, &nullSrv, g_nullHeap->GetCPUDescriptorHandleForHeapStart());

			if (!CreateRootSignature()) {
				return false;
			}
			if (!Shaders::PixelShader()) {
				TurnOff("pixel shader did not compile");
				return false;
			}
			if (!RenderAssets::Start(g_device.Get())) {
				TurnOff("asset worker did not start");
				return false;
			}
			if (!Gbuffer::Init(g_device.Get(), g_root.Get())) {
				Log::Warn("render: stage C unavailable; Snake is drawn with stage B");
			}
			// Command-list watching (scene depth, G-buffer, views, frame probe): the depth test and stage C need it.
			if (Cfg().render.erDepth || Cfg().render.probe || Cfg().render.stage == "gbuffer") {
				RenderDepth::Install(g_device.Get(), g_list.Get());
				RenderProbe::Install(g_device.Get(), g_list.Get());
				RenderBind::Install(g_device.Get(), g_list.Get());
			}
			g_swap = a_swap;
			g_ready = true;
			Log::Info("render: ready on device {} queue {} swap chain {} (present thread {})", static_cast<void*>(g_device.Get()), static_cast<void*>(g_queue.Get()),
				static_cast<void*>(a_swap), ::GetCurrentThreadId());
			return true;
		}

		void ReleaseTargets()
		{
			g_sceneTarget = nullptr;
			g_backBuffers.clear();
			g_depth.Reset();
			g_width = g_height = 0;
		}

		float OutputMode(DXGI_FORMAT a_format)
		{
			const auto space = D3D12Hooks::ColorSpace();
			if (d3d::Info(a_format).srgb) {
				return 1;
			}
			if (a_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
				return 3;
			}
			if (a_format == DXGI_FORMAT_R10G10B10A2_UNORM && space == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
				return 2;
			}
			return 0;
		}

		bool EnsureTargets(IDXGISwapChain* a_swap)
		{
			DXGI_SWAP_CHAIN_DESC desc{};
			if (!Check(a_swap->GetDesc(&desc), "swap chain GetDesc")) {
				return false;
			}
			const auto fmt = d3d::RtvFormat(desc.BufferDesc.Format);
			const auto mode = OutputMode(fmt);
			if (g_backBuffers.size() == desc.BufferCount && g_width == desc.BufferDesc.Width && g_height == desc.BufferDesc.Height && g_bbFormat == fmt && mode == g_outputMode) {
				return true;
			}
			WaitIdle();
			ReleaseTargets();
			if (desc.BufferCount == 0 || desc.BufferCount > kRtvSlots) {
				TurnOff(std::format("swap chain with {} buffers", desc.BufferCount));
				return false;
			}
			for (UINT i = 0; i < desc.BufferCount; ++i) {
				ComPtr<ID3D12Resource> bb;
				if (!Check(a_swap->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(bb.GetAddressOf())), "swap chain GetBuffer")) {
					ReleaseTargets();
					return false;
				}
				D3D12_RENDER_TARGET_VIEW_DESC rtv{};
				rtv.Format = fmt;
				rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				auto h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
				h.ptr += std::size_t(i) * g_rtvInc;
				g_device->CreateRenderTargetView(bb.Get(), &rtv, h);
				g_backBuffers.push_back(bb);
			}
			D3D12_RESOURCE_DESC dd{};
			dd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			dd.Width = desc.BufferDesc.Width;
			dd.Height = desc.BufferDesc.Height;
			dd.DepthOrArraySize = 1;
			dd.MipLevels = 1;
			dd.Format = DXGI_FORMAT_D32_FLOAT;
			dd.SampleDesc.Count = 1;
			dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			D3D12_CLEAR_VALUE clear{};
			clear.Format = DXGI_FORMAT_D32_FLOAT;
			clear.DepthStencil.Depth = 0.0f;
			const auto props = d3d::HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			if (!Check(g_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &dd, D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, __uuidof(ID3D12Resource),
						   reinterpret_cast<void**>(g_depth.GetAddressOf())),
					"depth buffer")) {
				ReleaseTargets();
				return false;
			}
			g_device->CreateDepthStencilView(g_depth.Get(), nullptr, g_dsvHeap->GetCPUDescriptorHandleForHeapStart());
			g_width = desc.BufferDesc.Width;
			g_height = desc.BufferDesc.Height;
			g_bbFormat = fmt;
			g_outputMode = mode;
			static const char* kModes[] = { "sRGB encode", "linear (sRGB view)", "HDR10 PQ", "scRGB" };
			Log::Info("render: back buffers {} x {}x{} {} ({}), colour space {}, output {}", desc.BufferCount, g_width, g_height, static_cast<int>(desc.BufferDesc.Format),
				d3d::Name(desc.BufferDesc.Format), static_cast<int>(D3D12Hooks::ColorSpace()), kModes[static_cast<int>(mode)]);
			return true;
		}

		ID3D12PipelineState* Pso(const RenderAssets::Mesh& a_mesh, std::uint32_t a_flags, bool a_mirrored)
		{
			const auto& rc = Cfg().render;
			const bool  xray = rc.xray > 0.0f;
			const bool  translucent = (a_flags & proto::kDrawTranslucent) != 0 || xray;
			const auto  cull = render::CullMode(a_flags);
			const bool  ccw = render::FrontCcw(a_mirrored, a_mesh.winding);
			std::uint64_t key = a_mesh.layoutHash;
			key ^= (std::uint64_t(cull) << 1 | std::uint64_t(ccw) << 3 | std::uint64_t(translucent) << 4 | std::uint64_t(xray) << 5 | std::uint64_t(g_rtFormat) << 8) *
			       0x9E3779B97F4A7C15ull;
			if (auto it = g_psos.find(key); it != g_psos.end()) {
				return it->second.Get();
			}
			if (g_badPsos.contains(key)) {
				return nullptr;
			}
			auto* vs = render::VertexShaderFor(a_mesh);
			auto  ps = Shaders::PixelShader();
			if (!vs || !ps) {
				g_badPsos.insert(key);
				return nullptr;
			}
			std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
			for (const auto& e : a_mesh.elements) {
				D3D12_INPUT_ELEMENT_DESC d{};
				d.SemanticName = e.semantic;
				d.SemanticIndex = e.semanticIndex;
				d.Format = static_cast<DXGI_FORMAT>(e.format);
				d.InputSlot = e.inputSlot;
				d.AlignedByteOffset = e.alignedByteOffset;
				d.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				layout.push_back(d);
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = g_root.Get();
			pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			auto& rt = pd.BlendState.RenderTarget[0];
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
			if (translucent) {
				rt.BlendEnable = TRUE;
				rt.SrcBlend = xray ? D3D12_BLEND_BLEND_FACTOR : D3D12_BLEND_SRC_ALPHA;
				rt.DestBlend = xray ? D3D12_BLEND_INV_BLEND_FACTOR : D3D12_BLEND_INV_SRC_ALPHA;
				rt.BlendOp = D3D12_BLEND_OP_ADD;
				rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
				rt.DestBlendAlpha = D3D12_BLEND_ONE;
				rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			}
			rt.LogicOp = D3D12_LOGIC_OP_NOOP;
			pd.SampleMask = UINT_MAX;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = cull;
			pd.RasterizerState.FrontCounterClockwise = ccw;
			pd.RasterizerState.DepthClipEnable = TRUE;
			pd.DepthStencilState.DepthEnable = TRUE;
			pd.DepthStencilState.DepthWriteMask = translucent ? D3D12_DEPTH_WRITE_MASK_ZERO : D3D12_DEPTH_WRITE_MASK_ALL;
			pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
			pd.InputLayout = { layout.data(), static_cast<UINT>(layout.size()) };
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = g_rtFormat;
			pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
			pd.SampleDesc.Count = 1;
			ComPtr<ID3D12PipelineState> pso;
			const HRESULT               hr = g_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso.GetAddressOf()));
			if (FAILED(hr)) {
				Log::Error("render: pipeline for layout {:#018x} failed: {}", a_mesh.layoutHash, d3d::Hr(hr));
				g_badPsos.insert(key);
				return nullptr;
			}
			Log::Info("render: pipeline {} for layout {:#018x} ({} elements, cull {}, front {}, {})", g_psos.size() + 1, a_mesh.layoutHash, layout.size(),
				static_cast<int>(cull), ccw ? "ccw" : "cw", translucent ? "blended" : "opaque");
			return (g_psos[key] = pso).Get();
		}

		// Elden Ring's scene depth for this frame's draws: SRV in the frame's last descriptor, the resource
		// moved to a pixel-shader-readable state (and back afterwards, see RecordDraws). False: no test.
		bool BindSceneDepth(FrameConstants& a_c, const RenderSnapshot& a_s, D3D12_CPU_DESCRIPTOR_HANDLE a_slot, RenderDepth::Target& a_t)
		{
			const auto& rc = Cfg().render;
			if (!rc.erDepth || !RenderDepth::Current(a_t)) {
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
			srv.Format = a_t.srvFormat;
			srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			srv.Texture2D.MipLevels = 1;
			g_device->CreateShaderResourceView(a_t.resource, &srv, a_slot);
			const auto& cam = a_s.camera;
			a_c.depth[0] = 1;
			a_c.depth[1] = rc.erDepthNear > 0 ? rc.erDepthNear : (cam.nearZ > 0.001f ? cam.nearZ : 0.1f);
			a_c.depth[2] = static_cast<float>(a_t.width) / static_cast<float>(g_width);
			a_c.depth[3] = static_cast<float>(a_t.height) / static_cast<float>(g_height);
			a_c.depth2[0] = cam.farZ > a_c.depth[1] ? cam.farZ : 10000.0f;
			a_c.depth2[1] = rc.erDepthBias;
			a_c.depth2[2] = a_t.clearDepth > 0.5f ? 1.0f : 0.0f;
			return true;
		}

		// The state to read ER's depth in from our pixel shader, given the one it was left in.
		D3D12_RESOURCE_STATES ReadState(D3D12_RESOURCE_STATES a_s)
		{
			if (a_s == D3D12_RESOURCE_STATE_DEPTH_WRITE || a_s == D3D12_RESOURCE_STATE_COMMON) {
				return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_DEPTH_READ;
			}
			return a_s | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;  // already read-only: just add PS reads
		}

		// Where a frame's draws go: the back buffer at Present (stage A), or Elden Ring's post-tonemap scene
		// colour right before its composite (stage B: the HUD then goes on top of Snake).
		struct DrawTarget
		{
			ID3D12Resource*             resource{ nullptr };
			D3D12_RESOURCE_STATES       state{ D3D12_RESOURCE_STATE_PRESENT };  // what it is in now (restored afterwards)
			D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
			DXGI_FORMAT                 format{ DXGI_FORMAT_UNKNOWN };
			float                       outputMode{ 0 };
		};

		bool BackBufferTarget(IDXGISwapChain* a_swap, DrawTarget& a_t)
		{
			ComPtr<IDXGISwapChain3> sc3;
			if (FAILED(a_swap->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(sc3.GetAddressOf())))) {
				TurnOff("swap chain has no IDXGISwapChain3");
				return false;
			}
			const UINT index = sc3->GetCurrentBackBufferIndex();
			if (index >= g_backBuffers.size()) {
				return false;
			}
			a_t.resource = g_backBuffers[index].Get();
			a_t.state = D3D12_RESOURCE_STATE_PRESENT;
			a_t.rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			a_t.rtv.ptr += std::size_t(index) * g_rtvInc;
			a_t.format = g_bbFormat;
			a_t.outputMode = g_outputMode;
			return true;
		}

		// Stages A/B: records the snapshot's draws into a_target, lit here. False: nothing recorded.
		bool RecordDraws(render::Recording& a_rec, const RenderSnapshot& a_s, const DrawTarget& a_target)
		{
			ID3D12Resource* bb = a_target.resource;
			const auto      rtv = a_target.rtv;
			const auto      dsv = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
			g_rtFormat = a_target.format;

			// The depth SRV lives in the slice's last descriptor.
			const auto depthCpu = a_rec.Cpu(a_rec.descLimit);
			const auto depthGpu = a_rec.Gpu(a_rec.descLimit);

			FrameConstants      fc{};
			RenderDepth::Target depth;
			render::FillFrame(fc, a_s, g_width, g_height, false);
			fc.output[0] = a_target.outputMode;
			const bool useDepth = BindSceneDepth(fc, a_s, depthCpu, depth);
			if (!useDepth) {
				g_device->CopyDescriptorsSimple(1, depthCpu, a_rec.nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			const auto fcVa = a_rec.Upload(&fc, sizeof(fc));
			if (!fcVa) {
				return false;
			}

			auto*                  list = a_rec.list;
			D3D12_RESOURCE_BARRIER pre[2]{};
			UINT                   npre = 0;
			const bool             rtBarrier = a_target.state != D3D12_RESOURCE_STATE_RENDER_TARGET;
			if (rtBarrier) {
				pre[npre++] = d3d::Transition(bb, a_target.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}
			const auto readState = useDepth ? ReadState(depth.state) : D3D12_RESOURCE_STATE_COMMON;
			const bool depthBarrier = useDepth && readState != depth.state;
			if (depthBarrier) {
				pre[npre++] = d3d::Transition(depth.resource, depth.state, readState, 0);
			}
			if (npre) {
				list->ResourceBarrier(npre, pre);
			}
			list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
			list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
			const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(g_width), static_cast<float>(g_height), 0, 1 };
			const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &sr);
			list->SetGraphicsRootSignature(g_root.Get());
			ID3D12DescriptorHeap* heaps[] = { g_gpuHeap.Get() };
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRootConstantBufferView(render::kRootFrame, fcVa);
			list->SetGraphicsRootConstantBufferView(render::kRootErView, fcVa);  // declared, unused (FrameCB.misc.w = 0)
			const float xray = std::clamp(Cfg().render.xray, 0.0f, 1.0f);
			const float blend[4] = { xray, xray, xray, xray };
			list->OMSetBlendFactor(blend);
			list->SetGraphicsRootDescriptorTable(render::kRootDepth, depthGpu);

			const auto fades = render::Fades(a_s);
			// Opaque first, then blended ones (no sorting among those yet).
			bool full = false;
			for (int pass = 0; pass < 2 && !full; ++pass) {
				for (std::size_t i = 0; i < a_s.draws.size(); ++i) {
					const auto& d = a_s.draws[i];
					if (((d.flags & proto::kDrawTranslucent) != 0) != (pass == 1)) {
						continue;
					}
					render::BoundDraw b;
					const auto        r = render::BindDraw(a_rec, a_s, i, fades[i], b);
					if (r == render::BindResult::kFull) {
						full = true;
						break;
					}
					if (r != render::BindResult::kOk) {
						++(r == render::BindResult::kMissingMesh ? g_drawsMissing : g_drawsBad);
						continue;
					}
					auto* pso = Pso(*b.mesh, d.flags, a_s.mirrored);
					if (!pso) {
						++g_drawsBad;
						continue;
					}
					list->SetPipelineState(pso);
					render::DrawMesh(list, d, *b.mesh);
					++g_drawsIssued;
				}
			}
			D3D12_RESOURCE_BARRIER post[2]{};
			UINT                   npost = 0;
			if (rtBarrier) {
				post[npost++] = d3d::Transition(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, a_target.state);
			}
			if (depthBarrier) {
				post[npost++] = d3d::Transition(depth.resource, readState, depth.state, 0);
			}
			if (npost) {
				list->ResourceBarrier(npost, post);
			}
			if (useDepth) {
				++g_depthFrames;
			}
			return true;
		}

		// er/RenderMarkers.h: premultiplied-alpha quads, depth-tested in the pixel shader against Elden Ring's scene depth.
		ID3D12PipelineState* MarkerPso(DXGI_FORMAT a_format)
		{
			if (g_markerFailed) {
				return nullptr;
			}
			if (auto it = g_markerPsos.find(static_cast<int>(a_format)); it != g_markerPsos.end()) {
				return it->second.Get();
			}
			auto vs = Shaders::MarkerShader(false);
			auto ps = Shaders::MarkerShader(true);
			if (!vs || !ps) {
				g_markerFailed = true;
				Log::Error("render: marker shaders did not compile; no throw arc / grenade / mine markers");
				return nullptr;
			}
			static const D3D12_INPUT_ELEMENT_DESC kLayout[] = {
				{ "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 20, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
				{ "TEXCOORD", 1, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 36, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
			};
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = g_root.Get();
			pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pd.InputLayout = { kLayout, static_cast<UINT>(std::size(kLayout)) };
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pd.RasterizerState.DepthClipEnable = FALSE;
			auto& rt = pd.BlendState.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D12_BLEND_ONE;
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ONE;
			rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
			pd.DepthStencilState.DepthEnable = FALSE;
			pd.SampleMask = UINT_MAX;
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = a_format;
			pd.SampleDesc.Count = 1;
			ComPtr<ID3D12PipelineState> pso;
			if (FAILED(g_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso.GetAddressOf())))) {
				g_markerFailed = true;
				Log::Error("render: marker pipeline for {} failed; no throw arc / grenade / mine markers", d3d::Name(a_format));
				return nullptr;
			}
			Log::Info("render: marker pipeline for {}", d3d::Name(a_format));
			return (g_markerPsos[static_cast<int>(a_format)] = pso).Get();
		}

		// The snapshot's markers into a_target. False: nothing recorded.
		bool RecordMarkers(render::Recording& a_rec, const RenderSnapshot& a_s, const DrawTarget& a_target)
		{
			if (a_s.markers.empty() || !a_s.camera.valid) {
				return false;
			}
			auto* pso = MarkerPso(a_target.format);
			if (!pso) {
				return false;
			}
			const auto& cam = a_s.camera;
			const auto  verts = RenderMarkers::Build(a_s.markers, cam.pos, Normalize(cam.right), Normalize(cam.up));
			if (verts.empty()) {
				return false;
			}
			const auto depthCpu = a_rec.Cpu(a_rec.descLimit);
			const auto depthGpu = a_rec.Gpu(a_rec.descLimit);
			FrameConstants      fc{};
			RenderDepth::Target depth;
			render::FillFrame(fc, a_s, g_width, g_height, false);
			fc.output[0] = a_target.outputMode;
			const bool useDepth = BindSceneDepth(fc, a_s, depthCpu, depth);
			if (!useDepth) {
				g_device->CopyDescriptorsSimple(1, depthCpu, a_rec.nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			}
			const auto fcVa = a_rec.Upload(&fc, sizeof(fc));
			const auto vbBytes = static_cast<UINT>(verts.size() * sizeof(RenderMarkers::Vertex));
			const auto vbVa = fcVa ? a_rec.Upload(verts.data(), vbBytes) : 0;
			if (!fcVa || !vbVa) {
				return false;
			}
			auto*                  list = a_rec.list;
			D3D12_RESOURCE_BARRIER pre[2]{};
			UINT                   npre = 0;
			const bool             rtBarrier = a_target.state != D3D12_RESOURCE_STATE_RENDER_TARGET;
			if (rtBarrier) {
				pre[npre++] = d3d::Transition(a_target.resource, a_target.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
			}
			const auto readState = useDepth ? ReadState(depth.state) : D3D12_RESOURCE_STATE_COMMON;
			const bool depthBarrier = useDepth && readState != depth.state;
			if (depthBarrier) {
				pre[npre++] = d3d::Transition(depth.resource, depth.state, readState, 0);
			}
			if (npre) {
				list->ResourceBarrier(npre, pre);
			}
			list->OMSetRenderTargets(1, &a_target.rtv, FALSE, nullptr);
			const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(g_width), static_cast<float>(g_height), 0, 1 };
			const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &sr);
			list->SetGraphicsRootSignature(g_root.Get());
			ID3D12DescriptorHeap* heaps[] = { g_gpuHeap.Get() };
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRootConstantBufferView(render::kRootFrame, fcVa);
			list->SetGraphicsRootConstantBufferView(render::kRootErView, fcVa);  // declared, unused
			list->SetGraphicsRootDescriptorTable(render::kRootDepth, depthGpu);
			list->SetPipelineState(pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			const D3D12_VERTEX_BUFFER_VIEW vbv{ vbVa, vbBytes, static_cast<UINT>(sizeof(RenderMarkers::Vertex)) };
			list->IASetVertexBuffers(0, 1, &vbv);
			list->DrawInstanced(static_cast<UINT>(verts.size()), 1, 0, 0);
			D3D12_RESOURCE_BARRIER post[2]{};
			UINT                   npost = 0;
			if (rtBarrier) {
				post[npost++] = d3d::Transition(a_target.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, a_target.state);
			}
			if (depthBarrier) {
				post[npost++] = d3d::Transition(depth.resource, readState, depth.state, 0);
			}
			if (npost) {
				list->ResourceBarrier(npost, post);
			}
			return true;
		}

		// One submission of our own list on Elden Ring's queue: queued uploads, then whatever a_record
		// records. Locked by the caller. a_viaOriginal: we are inside the ExecuteCommandLists detour, so
		// submit without passing through it again. Returns a_record's result.
		template <class F>
		bool Submit(bool a_viaOriginal, F&& a_record)
		{
			const UINT idx = g_ctxIndex;
			auto&      ctx = g_ctx[idx];
			if (!d3d::WaitFence(g_fence.Get(), g_event, ctx.fence, 100)) {
				++g_skipped;  // the GPU is far behind: skip rather than stall Elden Ring
				return false;
			}
			ctx.keep.clear();
			if (FAILED(ctx.alloc->Reset()) || !Check(g_list->Reset(ctx.alloc.Get(), nullptr), "command list Reset")) {
				return false;
			}
			UploadCursor up{ idx * kUploadPerFrame, (idx + 1) * kUploadPerFrame, idx * kUploadPerFrame };
			RenderAssets::RecordUploads(g_list.Get(), ctx.keep, std::uint64_t(std::max(1, Cfg().render.uploadMbPerFrame)) << 20);
			render::Recording rec;
			rec.device = g_device.Get();
			rec.list = g_list.Get();
			rec.root = g_root.Get();
			rec.heap = g_gpuHeap.Get();
			rec.srvInc = g_srvInc;
			rec.descBase = idx * kDescPerFrame;
			rec.descLimit = kDescPerFrame - 1;  // the last one: the forward stages' depth SRV
			rec.nullSrv = g_nullHeap->GetCPUDescriptorHandleForHeapStart();
			rec.uploadPtr = g_uploadPtr;
			rec.uploadVa = g_uploadVa;
			rec.up = &up;
			rec.keep = &ctx.keep;
			const bool recorded = a_record(rec);
			if (!Check(g_list->Close(), "command list Close")) {
				return false;
			}
			ID3D12CommandList* lists[] = { g_list.Get() };
			if (a_viaOriginal) {
				D3D12Hooks::ExecuteOriginal(g_queue.Get(), 1, lists);
			} else {
				g_queue->ExecuteCommandLists(1, lists);
			}
			ctx.fence = ++g_fenceValue;
			g_queue->Signal(g_fence.Get(), ctx.fence);
			Gbuffer::SetSubmitFence(ctx.fence);
			RenderView::SetSubmitFence(ctx.fence);
			RenderDepth::SetSubmitFence(ctx.fence);
			g_ctxIndex = (g_ctxIndex + 1) % kFrames;
			return recorded;
		}

		void FirstFrameLog(const RenderSnapshot& a_s, const char* a_where)
		{
			if (++g_framesDrawn == 1) {
				Log::Info("render: first frame drawn ({} draws in the snapshot, into {})", a_s.draws.size(), a_where);
			}
		}

		// Stage A/B submission into a_target.
		bool SubmitForward(const RenderSnapshot* a_snap, const DrawTarget* a_target, bool a_viaOriginal)
		{
			const bool drew = Submit(a_viaOriginal, [&](render::Recording& a_rec) { return a_snap && a_target && RecordDraws(a_rec, *a_snap, *a_target); });
			if (drew) {
				FirstFrameLog(*a_snap,
					a_target->resource == g_sceneTarget ? "Elden Ring's scene colour before its HUD (stage B)" : "the back buffer at Present (stage A)");
			}
			return drew;
		}

		void LogStats()
		{
			const auto now = ::GetTickCount64();
			if (now - g_lastStatLog > 30000) {
				g_lastStatLog = now;
				Log::Info("render: {} presents, {} frames drawn ({} into the G-buffer, {} before the HUD, {} stage C fallbacks), {} draws ({} missing mesh, {} bad), {} skipped, "
						  "{:.3f} ms CPU/frame; {}",
					g_presents.load(), g_framesDrawn.load(), g_gbufFrames.load(), g_sceneFrames.load(), g_fallbackFrames.load(), g_drawsIssued.load(), g_drawsMissing.load(),
					g_drawsBad.load(), g_skipped.load(), g_cpuMsAvg, RenderAssets::Describe());
			}
		}

		void OnPresent(IDXGISwapChain* a_swap, UINT)
		{
			++g_presents;
			if (g_off.load(std::memory_order_relaxed)) {
				return;
			}
			std::scoped_lock lock(g_lock);
			const auto       t0 = Link::Qpc();
			if (!g_ready) {
				if (!Init(a_swap)) {
					return;
				}
				g_presentThread = ::GetCurrentThreadId();
			}
			if (a_swap != g_swap) {
				return;  // some other swap chain (none expected in Elden Ring)
			}
			if (!EnsureTargets(a_swap)) {
				return;
			}
			// Already in this frame: stage B drew before the composite, or stage C put Snake in the G-buffer.
			const bool drawnBeforeHud = std::exchange(g_sceneDrawn, false) || g_gbufDrawn;
			g_gbufDone = g_velDone = g_gbufDrawn = false;
			Gbuffer::OnFence(g_fence->GetCompletedValue());
			RenderView::OnFence(g_fence->GetCompletedValue());
			RenderDepth::OnFence(g_fence->GetCompletedValue());
			RenderDepth::EndFrame(g_width, g_height);
			{
				ID3D12Resource* bbs[kRtvSlots]{};
				for (std::size_t i = 0; i < g_backBuffers.size(); ++i) {
					bbs[i] = g_backBuffers[i].Get();
				}
				RenderDepth::SetBackBuffers(bbs, static_cast<UINT>(g_backBuffers.size()));
				RenderDepth::Target t;
				RenderDepth::Current(t);
				RenderProbe::OnPresent(a_swap, bbs, static_cast<UINT>(g_backBuffers.size()), t.resource, {});
			}
			const auto snap = RenderScene::ForPresent();
			if (snap) {
				PacingProbe::OnRender(1, snap->erFrame);
			}
			const bool drawing = !drawnBeforeHud && snap && snap->draw && snap->camera.valid;
			DrawTarget target;
			const bool haveTarget = BackBufferTarget(a_swap, target);
			if ((drawing && haveTarget) || RenderAssets::HasPendingUploads()) {
				SubmitForward(drawing && haveTarget ? snap.get() : nullptr, drawing && haveTarget ? &target : nullptr, false);
			}
			// Markers that found no composite point this frame (stage A, or Elden Ring's frame not recognised): over it.
			if (!std::exchange(g_markersDrawn, false) && haveTarget && snap && !snap->markers.empty() && snap->camera.valid) {
				if (Submit(false, [&](render::Recording& a_rec) { return RecordMarkers(a_rec, *snap, target); })) {
					++g_markerPresentFrames;
				}
			}
			// The overlay (MGSV's HUD layer, reticle, corner text) goes over everything, Elden Ring's HUD included.
			if (haveTarget && !g_off.load(std::memory_order_relaxed)) {
				Overlay::OnPresent({ g_device.Get(), g_queue.Get(), target.resource, target.format, g_width, g_height, target.outputMode, Cfg().render.hdrPaperWhite });
			}
			const double ms = Link::QpcToMs(Link::Qpc() - t0);
			g_cpuMsAvg = g_cpuMsAvg * 0.98 + ms * 0.02;
			LogStats();
		}

		// Stage C refused (or recovered): log each change of reason once.
		void StageReason(const std::string& a_why)
		{
			if (a_why != g_stageWhy) {
				if (a_why.empty()) {
					Log::Info("render: stage C drawing into Elden Ring's G-buffer");
				} else {
					Log::Warn("render: stage C not possible ({}); Snake is drawn with stage B this frame", a_why);
				}
				g_stageWhy = a_why;
			}
		}

		// Elden Ring's post-tonemap scene colour (stage B's target) as a draw target. Locked.
		bool SceneColorTarget(DrawTarget& a_t)
		{
			RenderDepth::Target color;
			if (!(RenderDepth::SceneColor(color) && color.width == g_width && color.height == g_height)) {
				return false;
			}
			a_t.resource = color.resource;
			a_t.state = color.state;
			a_t.format = d3d::RtvFormat(color.format);
			if (a_t.format == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
				a_t.format = DXGI_FORMAT_R8G8B8A8_UNORM;
			}
			a_t.outputMode = d3d::Info(a_t.format).srgb ? 1.0f : 0.0f;
			a_t.rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			a_t.rtv.ptr += std::size_t(kRtvSlots - 1) * g_rtvInc;
			if (g_sceneTarget != color.resource || g_sceneRtvFormat != a_t.format) {
				D3D12_RENDER_TARGET_VIEW_DESC rd{};
				rd.Format = a_t.format;
				rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
				g_device->CreateRenderTargetView(color.resource, &rd, a_t.rtv);
				g_sceneTarget = color.resource;
				g_sceneRtvFormat = a_t.format;
				Log::Info("render: drawing into Elden Ring's scene colour {} (state {:#x}) before its composite: the HUD stays on top", static_cast<void*>(color.resource),
					static_cast<int>(color.state));
			}
			return true;
		}

		// Stage B: draw into ER's post-tonemap scene colour before its composite. Locked.
		bool DrawBeforeComposite(const std::shared_ptr<const RenderSnapshot>& a_snap)
		{
			DrawTarget t;
			if (!(a_snap && a_snap->draw && a_snap->camera.valid && SceneColorTarget(t))) {
				return false;
			}
			if (SubmitForward(a_snap.get(), &t, true)) {
				++g_sceneFrames;
				return true;
			}
			return false;
		}

		// The host's world markers (throw arc, grenades, mines) before the composite too: in the world, under the HUD.
		void MarkersBeforeComposite(const std::shared_ptr<const RenderSnapshot>& a_snap)
		{
			DrawTarget t;
			if (g_markersDrawn || !a_snap || a_snap->markers.empty() || !a_snap->camera.valid || !SceneColorTarget(t)) {
				return;
			}
			if (Submit(true, [&](render::Recording& a_rec) { return RecordMarkers(a_rec, *a_snap, t); })) {
				g_markersDrawn = true;
				++g_markerFrames;
			}
		}

		// Elden Ring's frame passes through here batch by batch. Up to three of our submissions go between
		// its lists: stage C after its last G-buffer list (Snake into the G-buffer, lit by Elden Ring) and
		// after its velocity pass (his motion vectors), stage B right before its composite (stage C's
		// fallback, or stage B itself). True: we submitted the batch ourselves.
		bool OnExecute(ID3D12CommandQueue* a_queue, UINT a_n, ID3D12CommandList* const* a_lists)
		{
			const auto& rc = Cfg().render;
			if (g_off.load(std::memory_order_relaxed) || !g_ready || a_queue != g_queue.Get() || rc.stage == "present") {
				RenderDepth::OnExecute(a_n, a_lists);
				RenderProbe::OnExecute(a_queue, a_n, a_lists);
				return false;
			}
			const bool stageC = rc.stage == "gbuffer";
			// Split points in this batch (after list g, after list v, before list k), -1 = none.
			const int g = stageC && !g_gbufDone ? RenderDepth::LastIndex(RenderDepth::kListGbuffer, a_n, a_lists) : -1;
			const int v = stageC && !g_velDone ? RenderDepth::LastIndex(RenderDepth::kListVelocity, a_n, a_lists) : -1;
			// Before the composite: Snake (stage B / C's fallback) until he is in, and the world markers when there are any
			// (most frames have none: a split per back-buffer batch for nothing would also redraw Snake in stage B).
			const bool wantScene = !g_sceneDrawn;
			const bool wantMarkers = !g_markersDrawn && RenderScene::HasMarkers();
			const int  k = wantScene || wantMarkers ? RenderDepth::CompositeIndex(a_n, a_lists) : -1;
			if (g < 0 && v < 0 && k < 0) {
				RenderDepth::OnExecute(a_n, a_lists);
				RenderProbe::OnExecute(a_queue, a_n, a_lists);
				return false;
			}
			std::scoped_lock lock(g_lock);
			const auto       t0 = Link::Qpc();
			const auto       snap = RenderScene::ForPresent();
			UINT             at = 0;
			auto             run = [&](UINT a_end) {  // Elden Ring's lists [at, a_end)
                if (a_end > at) {
                    RenderDepth::OnExecute(a_end - at, a_lists + at);
                    D3D12Hooks::ExecuteOriginal(a_queue, a_end - at, a_lists + at);
                    at = a_end;
                }
			};
			if (g >= 0) {
				run(static_cast<UINT>(g + 1));
				g_gbufDone = true;
				std::string why;
				const bool  drawing = snap && snap->draw && snap->camera.valid;
				if ((drawing || Gbuffer::WantsGbufferPoint(snap.get())) && Gbuffer::Ready(why)) {
					const bool drew = Submit(true, [&](render::Recording& a_rec) {
						return Gbuffer::RecordGbuffer(a_rec, snap.get(), static_cast<UINT>(g + 1), a_lists);
					});
					if (drew && drawing) {
						g_gbufDrawn = true;
						++g_gbufFrames;
						PacingProbe::OnRender(0, snap->erFrame);
						FirstFrameLog(*snap, "Elden Ring's G-buffer (stage C)");
					}
				}
				if (drawing) {
					StageReason(why);
				}
			}
			if (v >= 0 && static_cast<UINT>(v) >= at) {
				run(static_cast<UINT>(v + 1));
				g_velDone = true;
				if ((g_gbufDrawn && rc.velocity) || Gbuffer::WantsVelocityPoint()) {
					Submit(true, [&](render::Recording& a_rec) { return Gbuffer::RecordVelocity(a_rec, g_gbufDrawn ? snap.get() : nullptr); });
				}
			}
			if (k >= 0 && static_cast<UINT>(k) >= at) {
				run(static_cast<UINT>(k));
				if (wantScene) {
					if (g_gbufDrawn) {
						g_sceneDrawn = true;  // Snake is already in this frame (stage C)
					} else if (DrawBeforeComposite(snap)) {
						g_sceneDrawn = true;
						if (stageC && snap && snap->draw) {
							++g_fallbackFrames;
						}
					}
				}
				MarkersBeforeComposite(snap);
			}
			run(a_n);
			RenderProbe::OnExecute(a_queue, a_n, a_lists);
			const double ms = Link::QpcToMs(Link::Qpc() - t0);
			g_cpuMsAvg = g_cpuMsAvg * 0.98 + ms * 0.02;
			return true;
		}

		void OnResizeBefore(IDXGISwapChain* a_swap)
		{
			std::scoped_lock lock(g_lock);
			if (a_swap == g_swap) {
				WaitIdle();
				Overlay::WaitIdle();
				ReleaseTargets();
				RenderDepth::Reset();
				Gbuffer::Reset();
			}
		}

		void OnResizeAfter(IDXGISwapChain*, HRESULT) {}

		void OnDrawPre(float) { RenderScene::Capture(Host::Current()); }
	}

	bool Install()
	{
		if (!Cfg().render.enabled) {
			Log::Info("render: [Render] enabled = false");
			return false;
		}
		if (!Shaders::Init()) {
			return false;
		}
		D3D12Hooks::Callbacks cb;
		cb.present = &OnPresent;
		cb.resizeBefore = &OnResizeBefore;
		cb.resizeAfter = &OnResizeAfter;
		cb.execute = &OnExecute;
		cb.dummyDevice = [](ID3D12Device* a_device) { RenderPso::Install(a_device); };
		if (!D3D12Hooks::Install(cb)) {
			g_off = true;
			return false;
		}
		// From now on the asset worker owns the render ring (the link stops discarding it).
		Link::Get().SetRenderConsumer(true);
		return true;
	}

	void Start()
	{
		if (!Cfg().render.enabled || g_off.load()) {
			return;
		}
		RegisterTask(TaskGroup::kDrawPre, &OnDrawPre, "Render snapshot");
	}

	std::string Describe()
	{
		std::string s = std::format("render: {} | {} presents, {} frames drawn, {} draws issued, {} missing mesh, {} bad, {} skipped, {:.3f} ms CPU/frame, present thread {}\n",
			g_off.load() ? "OFF" : g_ready ? "ready" : "waiting for the swap chain/queue", g_presents.load(), g_framesDrawn.load(), g_drawsIssued.load(),
			g_drawsMissing.load(), g_drawsBad.load(), g_skipped.load(), g_cpuMsAvg, g_presentThread);
		s += std::format("  stage {} | back buffer {}x{} {} output mode {} | {} pipelines ({} failed) | {} frames depth-tested, {} before the HUD, {} into the G-buffer, {} stage C fallbacks{}\n  {}\n  {}\n  {}\n  {}\n  {}",
			Cfg().render.stage, g_width, g_height, d3d::Name(g_bbFormat), g_outputMode, g_psos.size(), g_badPsos.size(), g_depthFrames.load(), g_sceneFrames.load(),
			g_gbufFrames.load(), g_fallbackFrames.load(), g_stageWhy.empty() ? "" : " (stage C: " + g_stageWhy + ")", Gbuffer::Describe(), D3D12Hooks::Describe(),
			RenderAssets::Describe(), RenderScene::Describe(), RenderDepth::Describe() + "\n  " + RenderProbe::Describe() + "\n  " + RenderPso::Describe());
		s += std::format("\n  {}: {} frames before the HUD, {} at Present{}", RenderMarkers::Describe(), g_markerFrames.load(), g_markerPresentFrames.load(),
			g_markerFailed ? " (marker pipeline FAILED)" : "");
		return s;
	}

	std::string Command(std::string_view a_args)
	{
		std::istringstream in{ std::string(a_args) };
		std::string        verb, key, value;
		in >> verb >> key >> value;
		auto& r = Cfg().render;
		if (verb.empty() || verb == "status") {
			return Describe();
		}
		if (verb == "probe") {
			RenderProbe::Request(key == "shaders");
			return "frame probe requested: the next complete frame's passes go to PhantomRingHost.log (render probe: ...)\n" + RenderProbe::Describe();
		}
		if (verb == "psos") {
			return RenderPso::Describe(true);
		}
		if (verb == "gbuf") {
			const auto rest = a_args.substr(std::min(a_args.size(), a_args.find("gbuf") + 4));
			return Gbuffer::Command(rest);
		}
		if (verb == "keys") {
			return "render set <key> <value>: stage (gbuffer|post|present) debug_view (0 lit,1 albedo,2 normals,3 uv,4 facing,5 bones,6 srm; stage C: 1 grey albedo, 2 no normal map, 6 srm as albedo) "
				   "latency_frames cull (back|front|none) front_ccw flip_x xray (0..1 opacity) er_depth er_depth_bias er_depth_near day_night exposure light_intensity "
				   "draw_player draw_buddy albedo_srgb normal_xy (auto|rg|ag) enabled gbuffer_depth (finite|infinite) gbuffer_stencil gbuffer_light_set gbuffer_ao "
				   "gbuffer_translucency albedo_scale gloss_scale gloss_bias metal_scale alpha_test (auto|flag) alpha_ref velocity view_constants shadows velocity_scale (x,y) velocity_bias (x,y) near_fade (a,b) near_fade_other (a,b)";
		}
		if (verb != "set" || key.empty()) {
			return "usage: render [status] | render keys | render set <key> <value> | render probe [shaders] | render psos | render gbuf [peek [x y [n]]]";
		}
		auto b = [&] { return value == "1" || value == "true" || value == "on" || value == "yes"; };
		auto f = [&] { return static_cast<float>(std::atof(value.c_str())); };
		auto i = [&] { return static_cast<int>(std::strtol(value.c_str(), nullptr, 0)); };
		auto f2 = [&](float* a_out) {
			float x = 0, y = 0;
			if (std::sscanf(value.c_str(), "%f,%f", &x, &y) == 2) {
				a_out[0] = x;
				a_out[1] = y;
			}
		};
		if (key == "debug_view") r.debugView = i();
		else if (key == "latency_frames") r.latencyFrames = i();
		else if (key == "cull") r.cull = value;
		else if (key == "front_ccw") r.frontCcw = b();
		else if (key == "flip_x") r.flipX = b();
		else if (key == "exposure") r.exposure = f();
		else if (key == "light_intensity") r.lightIntensity = f();
		else if (key == "draw_player") r.drawPlayer = b();
		else if (key == "draw_buddy") r.drawBuddy = b();
		else if (key == "albedo_srgb") r.albedoSrgb = b();
		else if (key == "normal_xy") r.normalXy = value;
		else if (key == "enabled") r.enabled = b();
		else if (key == "xray") r.xray = f();
		else if (key == "er_depth") r.erDepth = b();
		else if (key == "day_night") r.dayNight = b();
		else if (key == "stage" || key == "layer") r.stage = value == "scene" ? "post" : value;
		else if (key == "gbuffer_depth") r.gbufferDepth = value;
		else if (key == "gbuffer_stencil") r.gbufferStencil = i();
		else if (key == "gbuffer_light_set") r.gbufferLightSet = value == "auto" ? -1 : i();
		else if (key == "gbuffer_ao") r.gbufferAo = f();
		else if (key == "gbuffer_translucency") r.gbufferTranslucency = f();
		else if (key == "albedo_scale") r.albedoScale = f();
		else if (key == "gloss_scale") r.glossScale = f();
		else if (key == "gloss_bias") r.glossBias = f();
		else if (key == "metal_scale") r.metalScale = f();
		else if (key == "alpha_test") r.alphaTest = value;
		else if (key == "alpha_ref") r.alphaRef = f();
		else if (key == "velocity") r.velocity = b();
		else if (key == "view_constants") r.viewConstants = b();
		else if (key == "shadows") r.shadows = b();
		else if (key == "velocity_scale") f2(r.velocityScale);
		else if (key == "velocity_bias") f2(r.velocityBias);
		else if (key == "near_fade") f2(r.nearFade);
		else if (key == "near_fade_other") f2(r.nearFadeOther);
		else if (key == "occluder_fade") r.occluderFade = f();
		else if (key == "occluder_radius") r.occluderRadius = f();
		else if (key == "debug_offset") {
			float x = 0, y = 0, z = 0;
			std::sscanf(value.c_str(), "%f,%f,%f", &x, &y, &z);
			r.debugOffset[0] = x;
			r.debugOffset[1] = y;
			r.debugOffset[2] = z;
		}
		else if (key == "show_tarnished") {
			// The puppet stops re-hiding the Tarnished (then `flag render on` shows him), for side-by-side
			// lighting comparisons with debug_offset; off = the puppet hides him again next frame.
			Cfg().hideMode = b() ? "none" : "render_flag";
		}
		else if (key == "er_depth_bias") r.erDepthBias = f();
		else if (key == "er_depth_near") r.erDepthNear = f();
		else return std::format("unknown key {} (render keys)", key);
		Log::Info("render: console set {} = {}", key, value);
		return std::format("{} = {}", key, value);
	}
}
