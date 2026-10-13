#include "RenderShadow.h"

#include "Config.h"
#include "Log.h"
#include "RenderDepth.h"
#include "RenderPso.h"
#include "RenderView.h"
#include "Shaders.h"

#include <atomic>
#include <cmath>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace phantom::er::RenderShadow
{
	namespace
	{
		using d3d::ComPtr;

		constexpr UINT kAtlasSlots = 4;

		std::mutex                   g_lock;
		ComPtr<ID3D12Device>         g_device;
		ID3D12RootSignature*         g_root{ nullptr };
		ComPtr<ID3D12DescriptorHeap> g_dsvHeap;
		UINT                         g_dsvInc{ 0 };
		struct DsvFor
		{
			ID3D12Resource* atlas{ nullptr };
			std::uint32_t   generation{ 0 };  // RenderDepth's: a recreated atlas at a reused address needs a new DSV
		};
		DsvFor                       g_dsvFor[kAtlasSlots]{};
		std::unordered_set<std::uint32_t> g_skipLogged;  // atlas generations we said we skip

		std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> g_psos;
		std::unordered_set<std::uint64_t>                             g_badPsos;
		RenderPso::Info                                               g_state;
		bool                                                          g_haveState{ false };
		std::atomic<std::uint64_t>                                    g_frames{ 0 }, g_draws{ 0 }, g_views{ 0 }, g_noState{ 0 };

		D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT a_slot)
		{
			auto h = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += std::size_t(a_slot) * g_dsvInc;
			return h;
		}

		// Our DSV on an atlas (D16 over its R16_TYPELESS); the slot, or -1 when all are taken. A slot whose atlas
		// RenderDepth let go (or recreated: another generation) is free again.
		int AtlasDsv(ID3D12Resource* a_atlas, std::uint32_t a_generation)
		{
			for (UINT i = 0; i < kAtlasSlots; ++i) {
				if (g_dsvFor[i].atlas == a_atlas && g_dsvFor[i].generation == a_generation) {
					return static_cast<int>(i);
				}
			}
			for (UINT i = 0; i < kAtlasSlots; ++i) {
				auto& s = g_dsvFor[i];
				if (!s.atlas || RenderDepth::AtlasGeneration(s.atlas) != s.generation) {
					D3D12_DEPTH_STENCIL_VIEW_DESC d{};
					d.Format = DXGI_FORMAT_D16_UNORM;
					d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
					g_device->CreateDepthStencilView(a_atlas, &d, Dsv(i));
					s = { a_atlas, a_generation };
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		ID3D12PipelineState* Pso(const RenderAssets::Mesh& a_mesh, std::uint32_t a_shFlags)
		{
			const bool    alpha = (a_shFlags & Shaders::kShAlphaTest) != 0;
			std::uint64_t key = a_mesh.layoutHash ^ (alpha ? 0x9E3779B97F4A7C15ull : 0);
			if (const auto it = g_psos.find(key); it != g_psos.end()) {
				return it->second.Get();
			}
			if (g_badPsos.contains(key)) {
				return nullptr;
			}
			auto* vs = render::VertexShaderFor(a_mesh);
			auto  ps = alpha ? Shaders::PixelShader(Shaders::Pixel::kShadow) : d3d::ComPtr<ID3DBlob>{};
			if (!vs || (alpha && !ps)) {
				g_badPsos.insert(key);
				return nullptr;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC    pd{};
			std::vector<D3D12_INPUT_ELEMENT_DESC> layout;
			// Shadows are cast by both sides (MGSV's thin parts), like Elden Ring's casters' culling below.
			render::PsoBase(pd, layout, a_mesh, g_root, vs, ps.Get(), D3D12_CULL_MODE_NONE, false);
			pd.NumRenderTargets = 0;
			pd.DSVFormat = DXGI_FORMAT_D16_UNORM;
			pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
			pd.DepthStencilState.DepthFunc = static_cast<D3D12_COMPARISON_FUNC>(g_state.depthFunc ? g_state.depthFunc : static_cast<decltype(g_state.depthFunc)>(D3D12_COMPARISON_FUNC_GREATER_EQUAL));
			pd.RasterizerState.DepthBias = g_state.depthBias;
			pd.RasterizerState.SlopeScaledDepthBias = g_state.slopeBias;
			pd.RasterizerState.DepthBiasClamp = g_state.biasClamp;
			pd.RasterizerState.DepthClipEnable = g_state.depthClip;
			ComPtr<ID3D12PipelineState> pso;
			const HRESULT               hr = g_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso.GetAddressOf()));
			if (FAILED(hr)) {
				Log::Error("render: shadow pipeline for layout {:#018x} failed: {}", a_mesh.layoutHash, d3d::Hr(hr));
				g_badPsos.insert(key);
				return nullptr;
			}
			Log::Info("render: shadow pipeline {} for layout {:#018x} ({}; Elden Ring's casters: {})", g_psos.size() + 1, a_mesh.layoutHash, alpha ? "alpha-tested" : "depth only",
				RenderPso::Formats(g_state));
			return (g_psos[key] = pso).Get();
		}
	}

	bool Init(ID3D12Device* a_device, ID3D12RootSignature* a_root)
	{
		std::scoped_lock lock(g_lock);
		g_device = a_device;
		g_root = a_root;
		D3D12_DESCRIPTOR_HEAP_DESC d{};
		d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		d.NumDescriptors = kAtlasSlots;
		if (FAILED(a_device->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(g_dsvHeap.GetAddressOf())))) {
			Log::Warn("render: shadow DSV heap failed: Snake casts no shadow");
			return false;
		}
		g_dsvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		return true;
	}

	std::uint32_t Record(render::Recording& a_rec, const RenderSnapshot& a_s, const std::vector<RenderBind::View>& a_views, UINT a_firstSlot)
	{
		std::scoped_lock lock(g_lock);
		if (!g_dsvHeap || a_views.empty() || !Cfg().render.shadows || a_s.dissolve > 0.3f) {
			return 0;  // (the death dissolve takes the shadow away early: shadows have no screen door)
		}
		if (!g_haveState) {
			// Elden Ring's caster state (its depth function and bias): learned once its pipelines exist.
			g_haveState = RenderPso::ShadowState(g_state);
			if (!g_haveState) {
				++g_noState;
				return 0;
			}
			Log::Info("render: shadows: Elden Ring's casters use {}", RenderPso::Formats(g_state));
		}
		auto*         list = a_rec.list;
		std::uint32_t drawn = 0;
		// One atlas at a time: its state to DEPTH_WRITE, every view of it, back.
		std::vector<ID3D12Resource*> atlases;
		for (const auto& v : a_views) {
			if (v.depth && std::find(atlases.begin(), atlases.end(), v.depth) == atlases.end()) {
				atlases.push_back(v.depth);
			}
		}
		for (auto* atlas : atlases) {
			RenderDepth::Target t;
			if (!RenderDepth::Atlas(atlas, t)) {
				continue;
			}
			std::vector<D3D12_VIEWPORT> vps;
			for (const auto& v : a_views) {
				if (v.depth == atlas) {
					vps.push_back(v.vp);
				}
			}
			if (!SunCascadeAtlas(t.width, t.height, vps.data(), vps.size()) && !RenderDepth::FullyClearedThisFrame(atlas)) {
				// Once per atlas generation -- but local-light atlases are let go and found again every few seconds (a new
				// generation each time): the first few at info, the rest at debug.
				if (g_skipLogged.insert(t.generation).second) {
					const auto line = std::format("render: shadows: not into atlas {} {}x{} ({} views this frame): not the sun cascades and not cleared "
					                              "every frame -- Elden Ring may reuse it, with Snake frozen in it",
						static_cast<void*>(atlas), t.width, t.height, vps.size());
					if (g_skipLogged.size() <= 4) {
						Log::Info("{}", line);
					} else {
						Log::Debug("{}", line);
					}
				}
				continue;
			}
			const int slot = AtlasDsv(atlas, t.generation);
			if (slot < 0) {
				continue;
			}
			std::vector<D3D12_RESOURCE_BARRIER> pre, post;
			render::Move(pre, post, atlas, t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			if (!pre.empty()) {
				list->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
			}
			const auto dsv = Dsv(static_cast<UINT>(slot));
			list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
			for (std::size_t vi = 0; vi < a_views.size(); ++vi) {
				const auto& v = a_views[vi];
				if (v.depth != atlas || v.vp.Width < 1.0f || v.vp.Height < 1.0f) {
					continue;
				}
				list->RSSetViewports(1, &v.vp);
				const D3D12_RECT sr{ static_cast<LONG>(v.vp.TopLeftX), static_cast<LONG>(v.vp.TopLeftY), static_cast<LONG>(v.vp.TopLeftX + v.vp.Width),
					static_cast<LONG>(v.vp.TopLeftY + v.vp.Height) };
				list->RSSetScissorRects(1, &sr);
				list->SetGraphicsRootConstantBufferView(render::kRootErView, RenderView::Slot(a_firstSlot + static_cast<UINT>(vi)));
				++g_views;
				const render::Fade solid{};  // shadows don't fade near the camera
				for (std::size_t i = 0; i < a_s.draws.size(); ++i) {
					const auto& d = a_s.draws[i];
					if (d.flags & proto::kDrawTranslucent) {
						continue;
					}
					render::BoundDraw b;
					const auto        r = render::BindDraw(a_rec, a_s, i, solid, b);
					if (r == render::BindResult::kFull) {
						break;
					}
					if (r != render::BindResult::kOk) {
						continue;
					}
					auto* pso = Pso(*b.mesh, b.flags);
					if (!pso) {
						continue;
					}
					list->SetPipelineState(pso);
					render::DrawMesh(list, d, *b.mesh);
					++drawn;
				}
			}
			if (!post.empty()) {
				list->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
			}
		}
		if (drawn) {
			if (++g_frames == 1) {
				Log::Info("render: shadows: first frame into Elden Ring's shadow atlas ({} views, {} draws)", a_views.size(), drawn);
			}
			g_draws += drawn;
		}
		return drawn;
	}

	void Reset()
	{
		std::scoped_lock lock(g_lock);
		for (auto& d : g_dsvFor) {
			d = {};
		}
	}

	bool SunCascadeAtlas(UINT a_width, UINT a_height, const D3D12_VIEWPORT* a_views, std::size_t a_count)
	{
		if (a_height == 0 || a_width != 2 * a_height) {
			return false;
		}
		const float h = static_cast<float>(a_height);
		for (std::size_t i = 0; i < a_count; ++i) {
			const auto& v = a_views[i];
			if (std::fabs(v.TopLeftX) < 0.5f && std::fabs(v.TopLeftY) < 0.5f && std::fabs(v.Width - h) < 0.5f && std::fabs(v.Height - h) < 0.5f) {
				return true;
			}
		}
		return false;
	}

	std::string Describe()
	{
		return std::format("shadows: {} frames, {} views, {} draws, {} pipelines ({} failed){}", g_frames.load(), g_views.load(), g_draws.load(), g_psos.size(), g_badPsos.size(),
			g_haveState ? std::format("; Elden Ring's casters: {}", RenderPso::Formats(g_state)) : std::format("; caster state not seen yet ({}x)", g_noState.load()));
	}
}
