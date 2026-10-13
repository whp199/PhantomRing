#include "Overlay.h"

#include "Config.h"
#include "D3D12Hooks.h"
#include "Hud.h"
#include "HudFrames.h"
#include "HudLayout.h"
#include "Link.h"
#include "Log.h"
#include "OverlayArt.h"
#include "Shaders.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>
#include <vector>

namespace phantom::er::Overlay
{
	namespace
	{
		using d3d::ComPtr;

		constexpr UINT kFrames = 3;
		constexpr UINT kSrvPerFrame = 32;  // HUD + reticle + text lines
		constexpr UINT kConstants = 16;

		constexpr char kShader[] = R"(
cbuffer Quad : register(b0)
{
	float4 gRect;    // NDC: x0 y0 (top left), x1 y1 (bottom right)
	float4 gUv;      // u0 v0 u1 v1
	float4 gParams;  // x opacity, y output (0 sRGB values, 1 sRGB view, 2 HDR10 PQ, 3 scRGB), z paper white nits, w straight alpha
	float4 gExtra;   // x 1 = colour bytes are an sRGB target's: encode(linear premultiplied), blended in linear light
};
Texture2D    tImage : register(t0);
SamplerState sLinear : register(s0);

struct VSOut
{
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOut VSMain(uint id : SV_VertexID)
{
	float2 t = float2(id & 1, id >> 1);
	VSOut o;
	o.pos = float4(lerp(gRect.xy, gRect.zw, t), 0, 1);
	o.uv = lerp(gUv.xy, gUv.zw, t);
	return o;
}

float3 SrgbToLinear(float3 c) { return lerp(pow((c + 0.055) / 1.055, 2.4), c / 12.92, step(c, 0.04045)); }
float3 Pq(float3 nits)
{
	float3 y = pow(saturate(nits / 10000.0), 0.1593017578125);
	return pow((0.8359375 + 18.8515625 * y) / (1.0 + 18.6875 * y), 78.84375);
}

float3 LinearToSrgb(float3 c)
{
	c = saturate(c);
	return lerp(1.055 * pow(c, 1.0 / 2.4) - 0.055, c * 12.92, step(c, 0.0031308));
}

float4 PSMain(VSOut i) : SV_Target
{
	float4 c = tImage.Sample(sLinear, i.uv);
	bool linearBlend = gExtra.x > 0.5;
	if (linearBlend) {
		c.rgb = SrgbToLinear(c.rgb);  // linear, premultiplied
	}
	if (gParams.w > 0.5) {
		c.rgb *= c.a;
	}
	c *= gParams.x;
	float mode = gParams.y;
	float3 straight = c.a > 1e-4 ? saturate(c.rgb / c.a) : float3(0, 0, 0);
	if (mode < 0.5) {
		// sRGB values in a UNORM target, composited the way Elden Ring's HUD is: premultiplied sRGB values.
		return linearBlend ? float4(LinearToSrgb(straight) * c.a, c.a) : c;
	}
	float3 lin = linearBlend ? straight : SrgbToLinear(straight);
	if (mode < 1.5) {
		return float4(lin * c.a, c.a);
	}
	if (mode < 2.5) {
		float3 r2020 = float3(dot(float3(0.6274, 0.3293, 0.0433), lin), dot(float3(0.0691, 0.9195, 0.0114), lin), dot(float3(0.0164, 0.0880, 0.8956), lin));
		return float4(Pq(r2020 * gParams.z) * c.a, c.a);
	}
	return float4(lin * (gParams.z / 80.0) * c.a, c.a);
}
)";

		struct QuadConstants
		{
			float rect[4];
			float uv[4];
			float params[4];
			float extra[4];
		};
		static_assert(sizeof(QuadConstants) == kConstants * 4);

		struct Tex
		{
			ComPtr<ID3D12Resource> res;
			UINT                   width{ 0 }, height{ 0 };
			bool                   uploaded{ false };
			std::uint64_t          lastUsed{ 0 };
		};

		struct FrameCtx
		{
			ComPtr<ID3D12CommandAllocator>      alloc;
			std::uint64_t                       fence{ 0 };
			std::vector<ComPtr<ID3D12Resource>> keep;
		};

		std::atomic<bool>                 g_off{ false };
		bool                              g_ready{ false };
		ComPtr<ID3D12Device>              g_device;
		ComPtr<ID3D12CommandQueue>        g_queue;
		ComPtr<ID3D12GraphicsCommandList> g_list;
		ComPtr<ID3D12Fence>               g_fence;
		HANDLE                            g_event{ nullptr };
		std::uint64_t                     g_fenceValue{ 0 };
		FrameCtx                          g_ctx[kFrames];
		UINT                              g_ctxIndex{ 0 };
		ComPtr<ID3D12DescriptorHeap>      g_rtvHeap, g_srvHeap;
		UINT                              g_srvInc{ 0 };
		ComPtr<ID3D12RootSignature>       g_root;
		ComPtr<ID3DBlob>                  g_vs, g_ps;
		std::unordered_map<int, ComPtr<ID3D12PipelineState>> g_psos;

		Tex                    g_hud;
		ComPtr<ID3D12Resource> g_hudUpload[kFrames];
		std::uint64_t          g_hudUploadBytes[kFrames]{};
		HudFrames::View        g_hudView;
		bool                   g_hudPending{ false };
		// g_hud holds a frame of the current guest session. Not g_hud.uploaded: that one means "the
		// texture has content" for Upload's barriers and must survive a new session.
		bool                   g_hudValid{ false };
		std::uint32_t          g_hudAttach{ ~0u };
		Tex                    g_reticle;
		int                    g_reticleSize{ 0 };
		std::unordered_map<std::string, Tex> g_text;
		std::unordered_map<std::string, int> g_textFailed;  // key -> frame it failed (retried later)

		std::uint64_t g_frame{ 0 };
		// Read by the console (task thread) while the Present thread works: atomics only.
		std::atomic<std::uint64_t> g_framesDrawn{ 0 }, g_hudUploads{ 0 }, g_panelsMade{ 0 }, g_skipped{ 0 }, g_hudFrame{ 0 };
		std::atomic<std::uint32_t> g_hudW{ 0 }, g_hudH{ 0 }, g_panelsCached{ 0 }, g_reticlePx{ 0 };
		bool          g_loggedFirst[3]{};

		void TurnOff(std::string_view a_why)
		{
			if (!g_off.exchange(true)) {
				Log::Error("overlay: {}; overlay off for this session", a_why);
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

		bool CreateRoot()
		{
			HMODULE d3d12 = ::GetModuleHandleW(L"d3d12.dll");
			auto    serialize = d3d12 ? reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12SerializeRootSignature"))) : nullptr;
			if (!serialize) {
				TurnOff("D3D12SerializeRootSignature missing");
				return false;
			}
			D3D12_DESCRIPTOR_RANGE range{};
			range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
			range.NumDescriptors = 1;
			D3D12_ROOT_PARAMETER params[2]{};
			params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			params[0].Constants.ShaderRegister = 0;
			params[0].Constants.Num32BitValues = kConstants;
			params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			params[1].DescriptorTable.NumDescriptorRanges = 1;
			params[1].DescriptorTable.pDescriptorRanges = &range;
			params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC desc{};
			desc.NumParameters = 2;
			desc.pParameters = params;
			desc.NumStaticSamplers = 1;
			desc.pStaticSamplers = &sampler;
			ComPtr<ID3DBlob> blob, error;
			if (FAILED(serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, blob.GetAddressOf(), error.GetAddressOf()))) {
				TurnOff(std::format("root signature: {}", error ? static_cast<const char*>(error->GetBufferPointer()) : "?"));
				return false;
			}
			return Check(g_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), __uuidof(ID3D12RootSignature),
							 reinterpret_cast<void**>(g_root.GetAddressOf())),
				"overlay CreateRootSignature");
		}

		bool Init(const Target& a_t)
		{
			g_device = a_t.device;
			g_queue = a_t.queue;
			for (auto& c : g_ctx) {
				if (!Check(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(c.alloc.GetAddressOf())),
						"overlay CreateCommandAllocator")) {
					return false;
				}
			}
			if (!Check(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_ctx[0].alloc.Get(), nullptr, __uuidof(ID3D12GraphicsCommandList),
						   reinterpret_cast<void**>(g_list.GetAddressOf())),
					"overlay CreateCommandList")) {
				return false;
			}
			g_list->Close();
			if (!Check(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(g_fence.GetAddressOf())), "overlay CreateFence")) {
				return false;
			}
			g_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
			D3D12_DESCRIPTOR_HEAP_DESC rd{};
			rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
			rd.NumDescriptors = 1;
			D3D12_DESCRIPTOR_HEAP_DESC sd{};
			sd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			sd.NumDescriptors = kFrames * kSrvPerFrame;
			sd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
			if (!Check(g_device->CreateDescriptorHeap(&rd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(g_rtvHeap.GetAddressOf())), "overlay RTV heap") ||
				!Check(g_device->CreateDescriptorHeap(&sd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(g_srvHeap.GetAddressOf())), "overlay SRV heap")) {
				return false;
			}
			g_srvInc = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			if (!CreateRoot()) {
				return false;
			}
			g_vs = Shaders::Compile(kShader, "VSMain", "vs_5_0");
			g_ps = Shaders::Compile(kShader, "PSMain", "ps_5_0");
			if (!g_vs || !g_ps) {
				TurnOff("overlay shaders did not compile");
				return false;
			}
			g_ready = true;
			Log::Info("overlay: ready (MGSV HUD layer {}, reticle {}, status text {}, messages {})", Cfg().overlay.hud, Cfg().overlay.reticle, Cfg().overlay.status,
				Cfg().overlay.messages);
			return true;
		}

		ID3D12PipelineState* Pso(DXGI_FORMAT a_format)
		{
			auto& pso = g_psos[static_cast<int>(a_format)];
			if (pso) {
				return pso.Get();
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
			pd.pRootSignature = g_root.Get();
			pd.VS = { g_vs->GetBufferPointer(), g_vs->GetBufferSize() };
			pd.PS = { g_ps->GetBufferPointer(), g_ps->GetBufferSize() };
			auto& rt = pd.BlendState.RenderTarget[0];
			rt.BlendEnable = TRUE;
			rt.SrcBlend = D3D12_BLEND_ONE;  // premultiplied
			rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
			rt.BlendOp = D3D12_BLEND_OP_ADD;
			rt.SrcBlendAlpha = D3D12_BLEND_ZERO;
			rt.DestBlendAlpha = D3D12_BLEND_ONE;
			rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
			rt.LogicOp = D3D12_LOGIC_OP_NOOP;
			rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN | D3D12_COLOR_WRITE_ENABLE_BLUE;
			pd.SampleMask = UINT_MAX;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			pd.RasterizerState.DepthClipEnable = TRUE;
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.NumRenderTargets = 1;
			pd.RTVFormats[0] = a_format;
			pd.SampleDesc.Count = 1;
			if (!Check(g_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso.GetAddressOf())), "overlay pipeline")) {
				return nullptr;
			}
			Log::Info("overlay: pipeline for {}", d3d::Name(a_format));
			return pso.Get();
		}

		bool CreateTexture(Tex& a_tex, UINT a_w, UINT a_h, FrameCtx& a_ctx)
		{
			if (a_tex.res && a_tex.width == a_w && a_tex.height == a_h) {
				return true;
			}
			if (a_tex.res) {
				a_ctx.keep.push_back(a_tex.res);  // the GPU may still sample it this frame
			}
			a_tex = {};
			D3D12_RESOURCE_DESC d{};
			d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
			d.Width = a_w;
			d.Height = a_h;
			d.DepthOrArraySize = 1;
			d.MipLevels = 1;
			d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			d.SampleDesc.Count = 1;
			const auto props = d3d::HeapProps(D3D12_HEAP_TYPE_DEFAULT);
			if (!Check(g_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
						   reinterpret_cast<void**>(a_tex.res.GetAddressOf())),
					std::format("overlay texture {}x{}", a_w, a_h))) {
				return false;
			}
			a_tex.width = a_w;
			a_tex.height = a_h;
			return true;
		}

		// Rows of a_src (a_srcPitch bytes apart) into a_tex through a_upload (grown as needed).
		bool Upload(Tex& a_tex, const std::uint8_t* a_src, UINT a_srcPitch, ComPtr<ID3D12Resource>& a_upload, std::uint64_t& a_uploadBytes)
		{
			const UINT          pitch = static_cast<UINT>(d3d::Align(std::uint64_t(a_tex.width) * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
			const std::uint64_t bytes = std::uint64_t(pitch) * a_tex.height;
			if (!a_upload || a_uploadBytes < bytes) {
				a_upload.Reset();
				const auto props = d3d::HeapProps(D3D12_HEAP_TYPE_UPLOAD);
				const auto desc = d3d::BufferDesc(bytes);
				if (!Check(g_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
							   reinterpret_cast<void**>(a_upload.GetAddressOf())),
						"overlay upload buffer")) {
					return false;
				}
				a_uploadBytes = bytes;
			}
			std::uint8_t* dst = nullptr;
			const D3D12_RANGE none{ 0, 0 };
			if (!Check(a_upload->Map(0, &none, reinterpret_cast<void**>(&dst)), "overlay upload Map")) {
				return false;
			}
			for (UINT y = 0; y < a_tex.height; ++y) {
				std::memcpy(dst + std::uint64_t(y) * pitch, a_src + std::uint64_t(y) * a_srcPitch, std::size_t(a_tex.width) * 4);
			}
			a_upload->Unmap(0, nullptr);

			auto* list = g_list.Get();
			if (a_tex.uploaded) {
				const auto b = d3d::Transition(a_tex.res.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				list->ResourceBarrier(1, &b);
			}
			D3D12_TEXTURE_COPY_LOCATION dl{};
			dl.pResource = a_tex.res.Get();
			dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			D3D12_TEXTURE_COPY_LOCATION sl{};
			sl.pResource = a_upload.Get();
			sl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			sl.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM, a_tex.width, a_tex.height, 1, pitch };
			list->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
			const auto b = d3d::Transition(a_tex.res.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			list->ResourceBarrier(1, &b);
			a_tex.uploaded = true;
			return true;
		}

		// A CPU-made picture into a texture of its own (static: a one-off upload buffer, kept for the frame).
		bool UploadImage(Tex& a_tex, const OverlayArt::Image& a_img, FrameCtx& a_ctx)
		{
			if (!CreateTexture(a_tex, a_img.width, a_img.height, a_ctx)) {
				return false;
			}
			ComPtr<ID3D12Resource> upload;
			std::uint64_t          bytes = 0;
			const bool             ok = Upload(a_tex, reinterpret_cast<const std::uint8_t*>(a_img.pixels.data()), a_img.width * 4, upload, bytes);
			if (upload) {
				a_ctx.keep.push_back(upload);
			}
			return ok;
		}

		std::uint32_t Accent(Hud::Tone a_tone)
		{
			switch (a_tone) {
			case Hud::Tone::kGood:
				return 0x7CC86E;
			case Hud::Tone::kWarn:
				return 0xF0BE3C;
			case Hud::Tone::kError:
				return 0xE6463C;
			case Hud::Tone::kInfo:
				break;
			}
			return 0xE8A33D;  // MGSV's amber
		}

		struct Quad
		{
			Tex*  tex;
			float x, y, w, h;  // back-buffer pixels
			float opacity;
			bool  straight;
			bool  linearBlend;
			float uv[4]{ 0, 0, 1, 1 };  // u0 v0 u1 v1 (the HUD layer is drawn in pieces: er/HudLayout.h)
		};

		void Draw(const Target& a_t, const std::vector<Quad>& a_quads, UINT a_ctxIndex)
		{
			auto* list = g_list.Get();
			auto* pso = Pso(a_t.rtvFormat);
			if (!pso) {
				return;
			}
			const auto toRt = d3d::Transition(a_t.backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
			list->ResourceBarrier(1, &toRt);
			D3D12_RENDER_TARGET_VIEW_DESC rv{};
			rv.Format = a_t.rtvFormat;
			rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			const auto rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			g_device->CreateRenderTargetView(a_t.backBuffer, &rv, rtv);
			list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(a_t.width), static_cast<float>(a_t.height), 0, 1 };
			const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(a_t.width), static_cast<LONG>(a_t.height) };
			list->RSSetViewports(1, &vp);
			list->RSSetScissorRects(1, &sr);
			list->SetGraphicsRootSignature(g_root.Get());
			ID3D12DescriptorHeap* heaps[] = { g_srvHeap.Get() };
			list->SetDescriptorHeaps(1, heaps);
			list->SetPipelineState(pso);
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			UINT slot = a_ctxIndex * kSrvPerFrame;
			for (const auto& q : a_quads) {
				if (slot >= (a_ctxIndex + 1) * kSrvPerFrame) {
					break;
				}
				D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
				sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
				sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				sv.Texture2D.MipLevels = 1;
				auto cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
				cpu.ptr += std::size_t(slot) * g_srvInc;
				g_device->CreateShaderResourceView(q.tex->res.Get(), &sv, cpu);
				auto gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
				gpu.ptr += std::uint64_t(slot) * g_srvInc;
				++slot;
				const float   W = static_cast<float>(a_t.width), H = static_cast<float>(a_t.height);
				QuadConstants c{};
				c.rect[0] = q.x / W * 2 - 1;
				c.rect[1] = 1 - q.y / H * 2;
				c.rect[2] = (q.x + q.w) / W * 2 - 1;
				c.rect[3] = 1 - (q.y + q.h) / H * 2;
				std::copy_n(q.uv, 4, c.uv);
				c.params[0] = std::clamp(q.opacity, 0.0f, 1.0f);
				c.params[1] = a_t.outputMode;
				c.params[2] = a_t.paperWhite;
				c.params[3] = q.straight ? 1.0f : 0.0f;
				c.extra[0] = q.linearBlend ? 1.0f : 0.0f;
				list->SetGraphicsRoot32BitConstants(0, kConstants, &c, 0);
				list->SetGraphicsRootDescriptorTable(1, gpu);
				list->DrawInstanced(4, 1, 0, 0);
			}
			const auto toPresent = d3d::Transition(a_t.backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
			list->ResourceBarrier(1, &toPresent);
		}

		void LogFirst(int a_kind, std::string_view a_what)
		{
			if (!g_loggedFirst[a_kind]) {
				g_loggedFirst[a_kind] = true;
				Log::Info("overlay: first {} drawn", a_what);
			}
		}
	}

	namespace
	{
		std::atomic<bool>      g_shotWanted{ false };
		ComPtr<ID3D12Resource> g_shotBuffer;
		std::string            g_shotStatus{ "no shot taken" };

		// Writes the copied back buffer (RGBA8/BGRA8 rows at a_pitch) as a half-size 24-bit BMP next to the DLL.
		void WriteShot(const std::uint8_t* a_px, UINT a_w, UINT a_h, UINT a_pitch, bool a_bgra)
		{
			HMODULE self = nullptr;
			wchar_t path[MAX_PATH]{};
			if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WriteShot),
					&self) ||
				!::GetModuleFileNameW(self, path, MAX_PATH)) {
				g_shotStatus = "shot: no module path";
				return;
			}
			std::wstring p(path);
			p = p.substr(0, p.find_last_of(L"\\/") + 1) + L"PhantomRingHost.shot.bmp";
			const UINT                w = a_w / 2, h = a_h / 2, row = (w * 3 + 3) & ~3u;
			std::vector<std::uint8_t> out(std::size_t(row) * h);
			for (UINT y = 0; y < h; ++y) {
				const std::uint8_t* src = a_px + std::size_t(a_h - 1 - y * 2) * a_pitch;  // BMP rows go bottom up
				std::uint8_t*       dst = out.data() + std::size_t(y) * row;
				for (UINT x = 0; x < w; ++x) {
					const std::uint8_t* s = src + std::size_t(x) * 8;
					dst[x * 3 + 0] = a_bgra ? s[0] : s[2];
					dst[x * 3 + 1] = s[1];
					dst[x * 3 + 2] = a_bgra ? s[2] : s[0];
				}
			}
			BITMAPFILEHEADER fh{};
			BITMAPINFOHEADER ih{};
			fh.bfType = 0x4D42;
			fh.bfOffBits = sizeof(fh) + sizeof(ih);
			fh.bfSize = fh.bfOffBits + static_cast<DWORD>(out.size());
			ih.biSize = sizeof(ih);
			ih.biWidth = static_cast<LONG>(w);
			ih.biHeight = static_cast<LONG>(h);
			ih.biPlanes = 1;
			ih.biBitCount = 24;
			HANDLE f = ::CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) {
				g_shotStatus = "shot: cannot write the file";
				return;
			}
			DWORD n = 0;
			::WriteFile(f, &fh, sizeof(fh), &n, nullptr);
			::WriteFile(f, &ih, sizeof(ih), &n, nullptr);
			::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &n, nullptr);
			::CloseHandle(f);
			g_shotStatus = std::format("shot written: PhantomRingHost.shot.bmp ({}x{}, frame {})", w, h, g_frame);
			Log::Info("overlay: {}", g_shotStatus);
		}

		// Called with the overlay's list open: copies the back buffer (PRESENT state before and after) for TakeShot.
		bool RecordShot(const Target& a_t, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& a_foot)
		{
			const auto desc = a_t.backBuffer->GetDesc();
			if (desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM && desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM && desc.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
				desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
				g_shotStatus = std::format("shot: back buffer format {} is not 8-bit RGBA", static_cast<int>(desc.Format));
				return false;
			}
			UINT64 bytes = 0;
			g_device->GetCopyableFootprints(&desc, 0, 1, 0, &a_foot, nullptr, nullptr, &bytes);
			if (!g_shotBuffer || g_shotBuffer->GetDesc().Width < bytes) {
				g_shotBuffer.Reset();
				const auto heap = d3d::HeapProps(D3D12_HEAP_TYPE_READBACK);
				const auto bd = d3d::BufferDesc(bytes);
				if (FAILED(g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
						reinterpret_cast<void**>(g_shotBuffer.GetAddressOf())))) {
					g_shotStatus = "shot: no readback buffer";
					return false;
				}
			}
			auto* list = g_list.Get();
			const auto toCopy = d3d::Transition(a_t.backBuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->ResourceBarrier(1, &toCopy);
			D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
			dst.pResource = g_shotBuffer.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint = a_foot;
			src.pResource = a_t.backBuffer;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = 0;
			list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
			const auto back = d3d::Transition(a_t.backBuffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
			list->ResourceBarrier(1, &back);
			return true;
		}
	}

	void RequestShot() { g_shotWanted.store(true, std::memory_order_release); }
	std::string ShotStatus() { return g_shotStatus; }

	void OnPresent(const Target& a_t)
	{
		const auto& cfg = Cfg().overlay;
		if (g_off.load(std::memory_order_relaxed) || !cfg.enabled || !a_t.device || !a_t.queue || !a_t.backBuffer || !a_t.width || !a_t.height) {
			return;
		}
		if (!g_ready && !Init(a_t)) {
			return;
		}
		if (a_t.device != g_device.Get() || a_t.queue != g_queue.Get()) {
			TurnOff("the renderer's device or queue changed");
			return;
		}
		++g_frame;
		const Hud::Model m = Hud::Snapshot();

		// Keep the guest's HUD triple buffer moving even while the layer is hidden: the reticle (auto)
		// needs to know whether frames arrive, and showing the layer must start from the newest one.
		if (m.attach != g_hudAttach) {
			g_hudAttach = m.attach;
			g_hudPending = false;
			g_hudValid = false;  // the previous guest's last frame must not show for the new one
			g_hudView = {};
		}
		if (m.readHud && cfg.hud) {
			HudFrames::View v;
			if (HudFrames::Acquire(m.attach, v)) {
				Hud::NoteHudFrame();
				g_hudView = v;
				g_hudPending = true;
			}
		}

		const bool showHud = m.hudOpacity > 0.0f && (g_hudPending || g_hudValid);
		bool       anyText = false;
		for (const auto& l : m.lines) {
			anyText |= l.opacity > 0.0f;
		}
		const bool shot = g_shotWanted.load(std::memory_order_acquire);
		if (!showHud && m.reticleOpacity <= 0.0f && !anyText && !shot) {
			return;
		}

		const UINT idx = g_ctxIndex;
		auto&      ctx = g_ctx[idx];
		if (!d3d::WaitFence(g_fence.Get(), g_event, ctx.fence, 100)) {
			++g_skipped;
			return;
		}
		ctx.keep.clear();
		if (FAILED(ctx.alloc->Reset()) || !Check(g_list->Reset(ctx.alloc.Get(), nullptr), "overlay command list Reset")) {
			return;
		}

		const float       W = static_cast<float>(a_t.width), H = static_cast<float>(a_t.height);
		std::vector<Quad> quads;

		// 1. MGSV's HUD layer, scaled to the back buffer (aspect kept, centred).
		if (showHud) {
			if (g_hudPending && g_hudView.pixels) {
				if (CreateTexture(g_hud, g_hudView.width, g_hudView.height, ctx) &&
					Upload(g_hud, g_hudView.pixels, g_hudView.width * 4, g_hudUpload[idx], g_hudUploadBytes[idx])) {
					++g_hudUploads;
					g_hudW = g_hud.width;
					g_hudH = g_hud.height;
					g_hudFrame = g_hudView.frame;
					g_hudPending = false;
					g_hudValid = true;
					g_hud.lastUsed = g_frame;
				}
			}
			if (g_hudValid) {
				// Next to Elden Ring's HUD: the weapon box and the notifications may sit apart from the rest (er/HudLayout.h).
				HudLayout::Settings ls;
				ls.scale = cfg.hudScale;
				std::copy_n(cfg.hudOffset, 2, ls.offset);
				if (!m.guestMenu) {  // MGSV's iDroid / pause menu is one full-screen picture: never cut
					std::copy_n(cfg.hudWeaponRegion, 4, ls.regions[0].uv);
					std::copy_n(cfg.hudWeaponOffset, 2, ls.regions[0].offset);
					std::copy_n(cfg.hudWeaponBarRegion, 4, ls.regions[1].uv);
					std::copy_n(cfg.hudWeaponOffset, 2, ls.regions[1].offset);
					std::copy_n(cfg.hudNotifyRegion, 4, ls.regions[2].uv);
					std::copy_n(cfg.hudNotifyOffset, 2, ls.regions[2].offset);
				}
				const auto pieces = HudLayout::Layout(W, H, static_cast<float>(g_hud.width), static_cast<float>(g_hud.height), ls);
				for (const auto& p : pieces) {
					Quad q{ &g_hud, p.x, p.y, p.w, p.h, m.hudOpacity, !g_hudView.premultiplied && g_hudView.pixels, g_hudView.srgbTarget };
					q.uv[0] = p.u0;
					q.uv[1] = p.v0;
					q.uv[2] = p.u1;
					q.uv[3] = p.v1;
					quads.push_back(q);
				}
				LogFirst(0, std::format("MGSV HUD layer ({}x{} -> {} pieces over {:.0f}x{:.0f})", g_hud.width, g_hud.height, pieces.size(), W, H));
			}
		}

		// 2. The fallback reticle.
		if (m.reticleOpacity > 0.0f) {
			const int size = std::max(16, static_cast<int>(std::lround(H * 0.045f * std::max(0.2f, cfg.reticleScale))));
			if (size != g_reticleSize || !g_reticle.uploaded) {
				OverlayArt::Image img;
				OverlayArt::Reticle(size, img);
				if (UploadImage(g_reticle, img, ctx)) {
					g_reticleSize = size;
					g_reticlePx = static_cast<std::uint32_t>(size);
				}
			}
			if (g_reticle.uploaded) {
				const float s = static_cast<float>(g_reticle.width);
				quads.push_back({ &g_reticle, std::floor((W - s) * 0.5f), std::floor((H - s) * 0.5f), s, s, m.reticleOpacity, false, false });
				LogFirst(1, std::format("fallback reticle ({} px)", g_reticle.width));
			}
		}

		// 3. Text panels, stacked from the configured corner.
		const int  px = std::clamp(static_cast<int>(std::lround(H * 0.0165f * std::max(0.3f, cfg.textScale))), 12, 160);
		const int  maxWidth = static_cast<int>(W * 0.42f);
		const bool right = cfg.corner.ends_with("right"), bottom = cfg.corner.starts_with("bottom");
		const float margin = std::round(H * 0.028f), gap = std::round(px * 0.35f);
		float       y = bottom ? H - margin : margin;
		for (const auto& l : m.lines) {
			if (l.opacity <= 0.0f) {
				continue;
			}
			const std::string key = std::format("{}\x1f{}\x1f{}", l.text, static_cast<int>(l.tone), px);
			auto              it = g_text.find(key);
			if (it == g_text.end()) {
				if (auto f = g_textFailed.find(key); f != g_textFailed.end() && g_frame - f->second < 600) {
					continue;
				}
				OverlayArt::Image img;
				Tex               tex;
				if (!OverlayArt::TextPanel(l.text, Accent(l.tone), px, maxWidth, img) || !UploadImage(tex, img, ctx)) {
					g_textFailed[key] = static_cast<int>(g_frame);
					Log::Warn("overlay: could not make the text panel \"{}\"", l.text);
					continue;
				}
				++g_panelsMade;
				it = g_text.emplace(key, std::move(tex)).first;
			}
			Tex& t = it->second;
			t.lastUsed = g_frame;
			const float w = static_cast<float>(t.width), h = static_cast<float>(t.height);
			const float x = right ? W - margin - w : margin;
			if (bottom) {
				y -= h;
			}
			quads.push_back({ &t, x, y, w, h, l.opacity, false, false });
			y = bottom ? y - gap : y + h + gap;
			LogFirst(2, std::format("text panel (\"{}\", {} px)", l.text, px));
		}
		// Panels unused for a while go (the GPU may still read one this frame: kept with the frame).
		for (auto it = g_text.begin(); it != g_text.end();) {
			if (g_frame - it->second.lastUsed > 300) {
				ctx.keep.push_back(it->second.res);
				it = g_text.erase(it);
			} else {
				++it;
			}
		}

		g_panelsCached = static_cast<std::uint32_t>(g_text.size());
		if (!quads.empty()) {
			Draw(a_t, quads, idx);
		}
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT shotFoot{};
		const bool                         shotRecorded = shot && RecordShot(a_t, shotFoot);
		if (shot) {
			g_shotWanted.store(false, std::memory_order_release);
		}
		if (!Check(g_list->Close(), "overlay command list Close")) {
			return;
		}
		ID3D12CommandList* lists[] = { g_list.Get() };
		D3D12Hooks::ExecuteOriginal(g_queue.Get(), 1, lists);
		ctx.fence = ++g_fenceValue;
		g_queue->Signal(g_fence.Get(), ctx.fence);
		if (shotRecorded && d3d::WaitFence(g_fence.Get(), g_event, ctx.fence, 1000)) {
			// A test aid: one wait of a frame on the Present thread, only when asked.
			const std::uint8_t* px = nullptr;
			const D3D12_RANGE   range{ 0, static_cast<SIZE_T>(shotFoot.Footprint.RowPitch) * shotFoot.Footprint.Height };
			if (SUCCEEDED(g_shotBuffer->Map(0, &range, reinterpret_cast<void**>(const_cast<std::uint8_t**>(&px)))) && px) {
				const auto f = shotFoot.Footprint.Format;
				WriteShot(px, shotFoot.Footprint.Width, shotFoot.Footprint.Height, shotFoot.Footprint.RowPitch,
					f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB);
				const D3D12_RANGE none{ 0, 0 };
				g_shotBuffer->Unmap(0, &none);
			}
		}
		g_ctxIndex = (g_ctxIndex + 1) % kFrames;
		g_framesDrawn += quads.empty() ? 0 : 1;
	}


	void WaitIdle()
	{
		if (!g_ready || !g_queue || !g_fence) {
			return;
		}
		const auto v = ++g_fenceValue;
		if (SUCCEEDED(g_queue->Signal(g_fence.Get(), v))) {
			d3d::WaitFence(g_fence.Get(), g_event, v, 2000);
		}
	}

	std::string Describe()
	{
		return std::format("overlay draw: {} | {} frames drawn, {} skipped | HUD texture {}x{} ({} uploads, guest frame {}) | reticle {} px | {} text panels cached, {} made",
			g_off.load() ? "OFF" : g_ready ? "ready" : (Cfg().overlay.enabled ? "waiting for the renderer" : "disabled"), g_framesDrawn.load(), g_skipped.load(),
			g_hudW.load(), g_hudH.load(), g_hudUploads.load(), g_hudFrame.load(), g_reticlePx.load(), g_panelsCached.load(), g_panelsMade.load());
	}
}
