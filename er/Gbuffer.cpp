#include "Gbuffer.h"

#include "Config.h"
#include "Log.h"
#include "RenderBind.h"
#include "RenderDepth.h"
#include "RenderShadow.h"
#include "RenderView.h"
#include "Shaders.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace phantom::er::Gbuffer
{
	namespace
	{
		using d3d::ComPtr;
		using render::BindResult;

		constexpr int  kTargets = RenderDepth::kGbufferTargets;
		constexpr int  kVelRtv = kTargets;  // our RTV slot for the velocity target
		constexpr UINT kRtvSlots = kTargets + 1;
		constexpr UINT kDsvWrite = 0, kDsvRead = 1;
		// Peek readback: up to 16x16 texels of each of 5 MRTs, the depth and stencil planes and velocity.
		constexpr int           kPeekMax = 16;
		constexpr int           kPeekSlots = kTargets + 3;  // + depth, stencil, velocity
		constexpr std::uint64_t kPeekSlotBytes = 256ull * kPeekMax;

		std::mutex                   g_lock;
		ComPtr<ID3D12Device>         g_device;
		ID3D12RootSignature*         g_root{ nullptr };
		ComPtr<ID3D12DescriptorHeap> g_rtvHeap, g_dsvHeap;
		UINT                         g_rtvInc{ 0 }, g_dsvInc{ 0 };
		ID3D12Resource*              g_rtvFor[kRtvSlots]{};
		DXGI_FORMAT                  g_rtvFmt[kRtvSlots]{};
		ID3D12Resource*              g_dsvFor[2]{};

		std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> g_psos;
		std::unordered_set<std::uint64_t>                             g_badPsos;

		// stats
		std::atomic<std::uint64_t> g_frames{ 0 }, g_velFrames{ 0 }, g_draws{ 0 }, g_velDraws{ 0 }, g_missing{ 0 }, g_bad{ 0 }, g_skippedTranslucent{ 0 };
		std::string                g_lastWhy;
		bool                       g_frameUsesEr{ false };  // this frame's G-buffer pass projected with Elden Ring's view constants
		std::uint64_t              g_erFrames{ 0 }, g_ownFrames{ 0 };

		// One peek: requested by the console, recorded at the next G-buffer (and velocity) injection,
		// decoded once the submission's fence passed.
		struct Peek
		{
			int           x{ 0 }, y{ 0 }, n{ 1 };
			bool          wanted{ false };
			bool          gbufRecorded{ false }, velRecorded{ false };
			std::uint64_t gbufFence{ 0 }, velFence{ 0 };
			bool          pendingFence{ false };  // the next SetSubmitFence belongs to: 1 gbuf, 2 vel
			int           pendingKind{ 0 };
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot[kPeekSlots]{};
			bool          have[kPeekSlots]{};
			std::uint64_t requestedFrame{ 0 };
			bool          learning{ false };  // an automatic peek at the Tarnished: learn his light set
		};
		Peek                   g_peek;
		ComPtr<ID3D12Resource> g_readback;
		std::string            g_peekResult{ "(no peek yet: render gbuf peek [x y] [n])" };
		int                    g_learnedLightSet{ -1 };
		int                    g_lightSetCandidate{ -1 };  // a different value seen once: taken when the next peek agrees
		std::uint64_t          g_lastLearn{ 0 }, g_learnTries{ 0 };
		constexpr int          kDefaultLightSet = 3;  // the Tarnished's in the tested Limgrave spots
		constexpr std::uint64_t kLearnEvery = 120;     // G-buffer frames between two tries

		D3D12_CPU_DESCRIPTOR_HANDLE Rtv(UINT a_slot)
		{
			auto h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += std::size_t(a_slot) * g_rtvInc;
			return h;
		}

		D3D12_CPU_DESCRIPTOR_HANDLE Dsv(UINT a_slot)
		{
			auto h = g_dsvHeap->GetCPUDescriptorHandleForHeapStart();
			h.ptr += std::size_t(a_slot) * g_dsvInc;
			return h;
		}

		void EnsureRtv(UINT a_slot, const RenderDepth::Target& a_t)
		{
			if (g_rtvFor[a_slot] == a_t.resource && g_rtvFmt[a_slot] == a_t.rtvFormat) {
				return;
			}
			D3D12_RENDER_TARGET_VIEW_DESC d{};
			d.Format = a_t.rtvFormat;
			d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			g_device->CreateRenderTargetView(a_t.resource, &d, Rtv(a_slot));
			g_rtvFor[a_slot] = a_t.resource;
			g_rtvFmt[a_slot] = a_t.rtvFormat;
		}

		void EnsureDsv(UINT a_slot, ID3D12Resource* a_depth)
		{
			if (g_dsvFor[a_slot] == a_depth) {
				return;
			}
			D3D12_DEPTH_STENCIL_VIEW_DESC d{};
			d.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			d.Flags = a_slot == kDsvRead ? (D3D12_DSV_FLAG_READ_ONLY_DEPTH | D3D12_DSV_FLAG_READ_ONLY_STENCIL) : D3D12_DSV_FLAG_NONE;
			g_device->CreateDepthStencilView(a_depth, &d, Dsv(a_slot));
			g_dsvFor[a_slot] = a_depth;
		}

		enum class Kind
		{
			kGbuffer,
			kVelocity,
		};

		ID3D12PipelineState* Pso(const RenderAssets::Mesh& a_mesh, std::uint32_t a_drawFlags, bool a_mirrored, Kind a_kind)
		{
			const auto    cull = render::CullMode(a_drawFlags);
			const bool    ccw = render::FrontCcw(a_mirrored, a_mesh.winding);
			std::uint64_t key = a_mesh.layoutHash;
			key ^= (std::uint64_t(cull) << 1 | std::uint64_t(ccw) << 3 | std::uint64_t(a_kind == Kind::kVelocity) << 4) * 0x9E3779B97F4A7C15ull;
			for (UINT i = 0; i < kRtvSlots; ++i) {
				key ^= std::uint64_t(g_rtvFmt[i]) << (8 + i * 8);
			}
			if (const auto it = g_psos.find(key); it != g_psos.end()) {
				return it->second.Get();
			}
			if (g_badPsos.contains(key)) {
				return nullptr;
			}
			auto* vs = render::VertexShaderFor(a_mesh);
			auto  ps = Shaders::PixelShader(a_kind == Kind::kGbuffer ? Shaders::Pixel::kGbuffer : Shaders::Pixel::kVelocity);
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
			pd.pRootSignature = g_root;
			pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
			pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
			pd.SampleMask = UINT_MAX;
			pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			pd.RasterizerState.CullMode = cull;
			pd.RasterizerState.FrontCounterClockwise = ccw;
			pd.RasterizerState.DepthClipEnable = TRUE;
			pd.DepthStencilState.DepthEnable = TRUE;
			pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;  // reversed Z
			pd.InputLayout = { layout.data(), static_cast<UINT>(layout.size()) };
			pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			pd.DSVFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
			pd.SampleDesc.Count = 1;
			if (a_kind == Kind::kGbuffer) {
				pd.NumRenderTargets = kTargets;
				for (int i = 0; i < kTargets; ++i) {
					pd.RTVFormats[i] = g_rtvFmt[i];
					pd.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
					pd.BlendState.RenderTarget[i].LogicOp = D3D12_LOGIC_OP_NOOP;
				}
				pd.BlendState.IndependentBlendEnable = TRUE;
				pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
				// Elden Ring's G-buffer pipelines write their object class into the stencil (REPLACE, mask 0xFF);
				// its lighting passes select pixels by those bits.
				pd.DepthStencilState.StencilEnable = TRUE;
				pd.DepthStencilState.StencilReadMask = 0xFF;
				pd.DepthStencilState.StencilWriteMask = 0xFF;
				D3D12_DEPTH_STENCILOP_DESC op{ D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_ALWAYS };
				pd.DepthStencilState.FrontFace = op;
				pd.DepthStencilState.BackFace = op;
			} else {
				pd.NumRenderTargets = 1;
				pd.RTVFormats[0] = g_rtvFmt[kVelRtv];
				pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_RED | D3D12_COLOR_WRITE_ENABLE_GREEN;
				pd.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
				pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
				// The same vertex shader wrote these depths in the G-buffer pass: a couple of ulps toward the
				// camera keeps Snake's own front surface passing.
				pd.RasterizerState.DepthBias = 2;
			}
			ComPtr<ID3D12PipelineState> pso;
			const HRESULT               hr = g_device->CreateGraphicsPipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(pso.GetAddressOf()));
			if (FAILED(hr)) {
				Log::Error("render: stage C {} pipeline for layout {:#018x} failed: {}", a_kind == Kind::kGbuffer ? "G-buffer" : "velocity", a_mesh.layoutHash, d3d::Hr(hr));
				g_badPsos.insert(key);
				return nullptr;
			}
			Log::Info("render: stage C {} pipeline {} for layout {:#018x} (cull {}, front {})", a_kind == Kind::kGbuffer ? "G-buffer" : "velocity", g_psos.size() + 1,
				a_mesh.layoutHash, static_cast<int>(cull), ccw ? "ccw" : "cw");
			return (g_psos[key] = pso).Get();
		}

		// a_from -> a_to for a resource we restore afterwards; a no-op barrier is skipped.
		void Move(std::vector<D3D12_RESOURCE_BARRIER>& a_pre, std::vector<D3D12_RESOURCE_BARRIER>& a_post, ID3D12Resource* a_res, D3D12_RESOURCE_STATES a_from,
			D3D12_RESOURCE_STATES a_to, UINT a_sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
		{
			if (a_from == a_to) {
				return;
			}
			a_pre.push_back(d3d::Transition(a_res, a_from, a_to, a_sub));
			a_post.push_back(d3d::Transition(a_res, a_to, a_from, a_sub));
		}

		// Records copies of the peek box of a_res (subresource a_sub, in COPY_SOURCE) into readback slot a_slot.
		void CopyPeek(ID3D12GraphicsCommandList* a_list, int a_slot, ID3D12Resource* a_res, UINT a_sub)
		{
			const auto                         desc = a_res->GetDesc();
			D3D12_PLACED_SUBRESOURCE_FOOTPRINT f{};
			UINT                               rows = 0;
			UINT64                             rowBytes = 0, total = 0;
			g_device->GetCopyableFootprints(&desc, a_sub, 1, 0, &f, &rows, &rowBytes, &total);
			const int n = std::clamp(g_peek.n, 1, kPeekMax);
			const int x0 = std::clamp(g_peek.x - n / 2, 0, static_cast<int>(desc.Width) - n);
			const int y0 = std::clamp(g_peek.y - n / 2, 0, static_cast<int>(desc.Height) - n);
			f.Offset = kPeekSlotBytes * a_slot;
			f.Footprint.Width = n;
			f.Footprint.Height = n;
			f.Footprint.Depth = 1;
			f.Footprint.RowPitch = 256;
			D3D12_TEXTURE_COPY_LOCATION dst{};
			dst.pResource = g_readback.Get();
			dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
			dst.PlacedFootprint = f;
			D3D12_TEXTURE_COPY_LOCATION src{};
			src.pResource = a_res;
			src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
			src.SubresourceIndex = a_sub;
			const D3D12_BOX box{ static_cast<UINT>(x0), static_cast<UINT>(y0), 0, static_cast<UINT>(x0 + n), static_cast<UINT>(y0 + n), 1 };
			a_list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
			g_peek.foot[a_slot] = f;
			g_peek.have[a_slot] = true;
		}

		float Half(std::uint16_t a_h)
		{
			const std::uint32_t s = (a_h >> 15) & 1, e = (a_h >> 10) & 31, m = a_h & 1023;
			float               v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? INFINITY : std::ldexp(static_cast<float>(m | 1024), static_cast<int>(e) - 25);
			return s ? -v : v;
		}

		float SrgbToLinear(float a_c) { return a_c <= 0.04045f ? a_c / 12.92f : std::pow((a_c + 0.055f) / 1.055f, 2.4f); }

		// Decodes the readback (fence passed): the centre texel of every target, and histograms of the
		// stencil and the light-set index over the box (Snake's or the Tarnished's values).
		void DecodePeek()
		{
			const std::uint8_t* base = nullptr;
			const D3D12_RANGE   range{ 0, kPeekSlotBytes * kPeekSlots };
			if (FAILED(g_readback->Map(0, &range, reinterpret_cast<void**>(const_cast<std::uint8_t**>(&base)))) || !base) {
				g_peekResult = "peek: readback Map failed";
				return;
			}
			const int   n = std::clamp(g_peek.n, 1, kPeekMax);
			auto        texel = [&](int a_slot, int a_x, int a_y, int a_bytes) { return base + g_peek.foot[a_slot].Offset + std::size_t(a_y) * 256 + std::size_t(a_x) * a_bytes; };
			const int   c = n / 2;
			if (g_peek.learning && g_peek.have[2] && g_peek.have[6]) {
				// The Tarnished's texels: stencil 0x50 (Elden Ring's character class) -> their light set.
				const int          bytes = g_peek.foot[6].Footprint.Format == DXGI_FORMAT_R8_TYPELESS || g_peek.foot[6].Footprint.Format == DXGI_FORMAT_R8_UINT ? 1 : 4;
				std::map<int, int> sets;
				int                chars = 0;
				for (int y = 0; y < n; ++y) {
					for (int x = 0; x < n; ++x) {
						if (*texel(6, x, y, bytes) == (Cfg().render.gbufferStencil & 0xFF)) {
							++chars;
							++sets[texel(2, x, y, 4)[3]];
						}
					}
				}
				int best = -1, most = 0;
				for (const auto& [v, k] : sets) {
					if (k > most) {
						best = v;
						most = k;
					}
				}
				if (chars * 3 >= n * n && best >= 0 && most * 2 > chars) {
					// A change needs two peeks in a row (120 frames apart) that agree: a passing state of the Tarnished's
					// (a respawn, an effect) must not light Snake for the whole area.
					if (best != g_learnedLightSet && (best == g_lightSetCandidate || g_learnedLightSet < 0)) {
						Log::Info("render: stage C: the Tarnished's G-buffer texels carry light set {} ({} of {} character texels at ({}, {})): Snake uses it", best, most, n * n,
							g_peek.x, g_peek.y);
						g_learnedLightSet = best;
					}
					g_lightSetCandidate = best;
				}
				const D3D12_RANGE none{ 0, 0 };
				g_readback->Unmap(0, &none);
				return;
			}
			std::string s = std::format("peek at ({}, {}) box {}x{}, requested at frame {}:", g_peek.x, g_peek.y, n, n, g_peek.requestedFrame);
			if (g_peek.have[0]) {
				std::uint32_t v;
				std::memcpy(&v, texel(0, c, c, 4), 4);
				const float nx = (v & 1023) / 1023.0f * 2 - 1, ny = ((v >> 10) & 1023) / 1023.0f * 2 - 1, nz = ((v >> 20) & 1023) / 1023.0f * 2 - 1;
				s += std::format("\n  RT0 normal ({:.3f}, {:.3f}, {:.3f}) a {}", nx, ny, nz, v >> 30);
			}
			if (g_peek.have[1]) {
				const auto* p = texel(1, c, c, 4);
				s += std::format("\n  RT1 metal {:.3f} gloss {:.3f} translucency {:.3f} ao {:.3f}", p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f);
			}
			if (g_peek.have[2]) {
				const auto* p = texel(2, c, c, 4);
				s += std::format("\n  RT2 albedo linear ({:.3f}, {:.3f}, {:.3f}) srgb ({}, {}, {}) light set {}", SrgbToLinear(p[0] / 255.0f), SrgbToLinear(p[1] / 255.0f),
					SrgbToLinear(p[2] / 255.0f), p[0], p[1], p[2], p[3]);
			}
			if (g_peek.have[3]) {
				const auto* p = texel(3, c, c, 2);
				s += std::format("\n  RT3 ({}, {})", p[0], p[1]);
			}
			if (g_peek.have[4]) {
				std::uint16_t h[4];
				std::memcpy(h, texel(4, c, c, 8), 8);
				s += std::format("\n  RT4 ({:.3f}, {:.3f}, {:.3f}, {:.3f})", Half(h[0]), Half(h[1]), Half(h[2]), Half(h[3]));
			}
			if (g_peek.have[5]) {
				float d;
				std::memcpy(&d, texel(5, c, c, 4), 4);
				float lo = 1e9f, hi = -1e9f;
				for (int y = 0; y < n; ++y) {
					for (int x = 0; x < n; ++x) {
						float v;
						std::memcpy(&v, texel(5, x, y, 4), 4);
						lo = std::min(lo, v);
						hi = std::max(hi, v);
					}
				}
				s += std::format("\n  depth {:.6g} (box {:.6g}..{:.6g})", d, lo, hi);
			}
			if (g_peek.have[6]) {
				// The stencil plane's footprint is 1 byte per texel (R8_TYPELESS) under vkd3d-proton.
				const int                    bytes = g_peek.foot[6].Footprint.Format == DXGI_FORMAT_R8_TYPELESS || g_peek.foot[6].Footprint.Format == DXGI_FORMAT_R8_UINT ? 1 : 4;
				std::map<int, int>           hist;
				for (int y = 0; y < n; ++y) {
					for (int x = 0; x < n; ++x) {
						++hist[*texel(6, x, y, bytes)];
					}
				}
				s += std::format("\n  stencil centre {:#04x}, box (fmt {}):", *texel(6, c, c, bytes), static_cast<int>(g_peek.foot[6].Footprint.Format));
				for (const auto& [v, k] : hist) {
					s += std::format(" {:#04x}x{}", v, k);
				}
			}
			if (g_peek.have[2] && n > 1) {
				std::map<int, int> hist;
				for (int y = 0; y < n; ++y) {
					for (int x = 0; x < n; ++x) {
						++hist[texel(2, x, y, 4)[3]];
					}
				}
				s += "\n  light sets in the box:";
				for (const auto& [v, k] : hist) {
					s += std::format(" {}x{}", v, k);
				}
			}
			if (g_peek.have[7]) {
				std::uint16_t h[2];
				std::memcpy(h, texel(7, c, c, 4), 4);
				float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
				for (int y = 0; y < n; ++y) {
					for (int x = 0; x < n; ++x) {
						std::uint16_t v[2];
						std::memcpy(v, texel(7, x, y, 4), 4);
						for (int k = 0; k < 2; ++k) {
							lo[k] = std::min(lo[k], Half(v[k]));
							hi[k] = std::max(hi[k], Half(v[k]));
						}
					}
				}
				s += std::format("\n  velocity ({:.5f}, {:.5f}) box x {:.5f}..{:.5f} y {:.5f}..{:.5f}", Half(h[0]), Half(h[1]), lo[0], hi[0], lo[1], hi[1]);
			}
			const D3D12_RANGE none{ 0, 0 };
			g_readback->Unmap(0, &none);
			g_peekResult = s;
			Log::Info("render: {}", s);
		}
	}

	bool Init(ID3D12Device* a_device, ID3D12RootSignature* a_root)
	{
		std::scoped_lock lock(g_lock);
		g_device = a_device;
		g_root = a_root;
		auto heap = [&](D3D12_DESCRIPTOR_HEAP_TYPE a_type, UINT a_n, ComPtr<ID3D12DescriptorHeap>& a_out) {
			D3D12_DESCRIPTOR_HEAP_DESC d{};
			d.Type = a_type;
			d.NumDescriptors = a_n;
			return SUCCEEDED(a_device->CreateDescriptorHeap(&d, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(a_out.GetAddressOf())));
		};
		if (!heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvSlots, g_rtvHeap) || !heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 2, g_dsvHeap)) {
			Log::Error("render: stage C descriptor heaps failed; stage C off");
			return false;
		}
		g_rtvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		if (!RenderView::Init(a_device)) {
			Log::Warn("render: stage C keeps its own camera matrices (no view constant copy)");
		}
		RenderShadow::Init(a_device, a_root);
		g_dsvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
		const auto props = d3d::HeapProps(D3D12_HEAP_TYPE_READBACK);
		const auto desc = d3d::BufferDesc(kPeekSlotBytes * kPeekSlots);
		if (FAILED(a_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
				reinterpret_cast<void**>(g_readback.GetAddressOf())))) {
			Log::Warn("render: stage C readback buffer failed: `render gbuf peek` unavailable");
		}
		return true;
	}

	bool Ready(std::string& a_why)
	{
		if (!g_rtvHeap) {
			a_why = "stage C not initialised";
			return false;
		}
		RenderDepth::Target gb[kTargets];
		if (!RenderDepth::Gbuffer(gb, &a_why)) {
			return false;
		}
		RenderDepth::Target depth;
		if (!RenderDepth::Current(depth)) {
			a_why = "scene depth state unknown";
			return false;
		}
		return true;
	}

	bool WantsGbufferPoint(const RenderSnapshot* a_snap)
	{
		std::scoped_lock lock(g_lock);
		if (g_peek.wanted) {
			return !g_peek.gbufRecorded;
		}
		// Time to learn the light set from the visible Tarnished?
		++g_learnTries;
		return Cfg().render.gbufferLightSet < 0 && a_snap && a_snap->tarnishedVisible && g_readback && g_learnTries - g_lastLearn > kLearnEvery;
	}

	int LightSet()
	{
		const int fixed = Cfg().render.gbufferLightSet;
		if (fixed >= 0) {
			return std::min(fixed, 255);
		}
		std::scoped_lock lock(g_lock);
		return g_learnedLightSet >= 0 ? g_learnedLightSet : kDefaultLightSet;
	}

	bool WantsVelocityPoint()
	{
		std::scoped_lock lock(g_lock);
		return g_peek.wanted && g_peek.gbufRecorded && !g_peek.velRecorded && g_peek.pendingKind == 0;
	}

	bool RecordGbuffer(render::Recording& a_rec, const RenderSnapshot* a_snap, UINT a_count, ID3D12CommandList* const* a_lists)
	{
		std::scoped_lock    lock(g_lock);
		RenderDepth::Target gb[kTargets];
		RenderDepth::Target depth;
		std::string         why;
		if (!g_rtvHeap || !RenderDepth::Gbuffer(gb, &why) || !RenderDepth::Current(depth)) {
			g_lastWhy = why.empty() ? "scene depth unknown" : why;
			return false;
		}
		const bool drawing = a_snap && a_snap->draw && a_snap->camera.valid;
		if (!g_peek.wanted && Cfg().render.gbufferLightSet < 0 && a_snap && a_snap->tarnishedVisible && g_readback && g_learnTries - g_lastLearn > kLearnEvery) {
			g_lastLearn = g_learnTries;
			g_peek = {};
			g_peek.x = std::clamp(static_cast<int>((a_snap->tarnishedNdc[0] * 0.5f + 0.5f) * depth.width), 0, static_cast<int>(depth.width) - 1);
			g_peek.y = std::clamp(static_cast<int>((0.5f - a_snap->tarnishedNdc[1] * 0.5f) * depth.height), 0, static_cast<int>(depth.height) - 1);
			g_peek.n = 6;
			g_peek.wanted = true;
			g_peek.learning = true;
		}
		const bool peek = g_peek.wanted && !g_peek.gbufRecorded && g_readback;
		if (!drawing && !peek) {
			return false;
		}
		for (int i = 0; i < kTargets; ++i) {
			EnsureRtv(i, gb[i]);
		}
		EnsureDsv(kDsvWrite, depth.resource);
		D3D12_RESOURCE_STATES stencilState = depth.state;
		RenderDepth::StencilPlane(stencilState);

		auto* list = a_rec.list;

		// Elden Ring's own view constants for this frame (its camera with jitter, its shadow cascades),
		// copied before anything else so every draw below can read them.
		RenderBind::View              gbView;
		std::vector<RenderBind::View> shadowViews;
		ID3D12DescriptorHeap*         erHeap = nullptr;
		g_frameUsesEr = false;
		if (drawing && Cfg().render.viewConstants && RenderBind::Collect(a_count, a_lists, gbView, shadowViews, erHeap)) {
			std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> cbs{ gbView.cb };
			for (const auto& v : shadowViews) {
				if (cbs.size() < RenderView::kSlots) {
					cbs.push_back(v.cb);
				}
			}
			shadowViews.resize(cbs.size() - 1);
			if (RenderView::RecordCopies(list, erHeap, cbs.data(), static_cast<UINT>(cbs.size()), a_snap->camera.pos, a_snap->erFrame)) {
				g_frameUsesEr = RenderView::MainTrusted();
			}
		}
		++(g_frameUsesEr ? g_erFrames : g_ownFrames);

		std::vector<D3D12_RESOURCE_BARRIER> pre, post;
		for (int i = 0; i < kTargets; ++i) {
			Move(pre, post, gb[i].resource, gb[i].state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		}
		Move(pre, post, depth.resource, depth.state, D3D12_RESOURCE_STATE_DEPTH_WRITE, 0);
		Move(pre, post, depth.resource, stencilState, D3D12_RESOURCE_STATE_DEPTH_WRITE, 1);
		if (!pre.empty()) {
			list->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
		}

		std::uint32_t drawn = 0;
		if (drawing) {
			const auto& s = *a_snap;
			render::FrameConstants fc{};
			render::FillFrame(fc, s, depth.width, depth.height, true);
			fc.misc[3] = g_frameUsesEr ? 1.0f : 0.0f;
			fc.gbuf[0] = static_cast<float>(Cfg().render.gbufferLightSet >= 0 ? std::min(Cfg().render.gbufferLightSet, 255)
			                                                                   : (g_learnedLightSet >= 0 ? g_learnedLightSet : kDefaultLightSet)) /
			             255.0f;
			const auto fcVa = a_rec.Upload(&fc, sizeof(fc));
			if (fcVa && g_frameUsesEr && !shadowViews.empty()) {
				// Snake's shadow first (other targets), then the G-buffer.
				list->SetGraphicsRootSignature(g_root);
				ID3D12DescriptorHeap* heaps[] = { a_rec.heap };
				list->SetDescriptorHeaps(1, heaps);
				list->SetGraphicsRootConstantBufferView(render::kRootFrame, fcVa);
				a_rec.device->CopyDescriptorsSimple(1, a_rec.Cpu(a_rec.descUsed), a_rec.nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				list->SetGraphicsRootDescriptorTable(render::kRootDepth, a_rec.Gpu(a_rec.descUsed));
				++a_rec.descUsed;
				RenderShadow::Record(a_rec, s, shadowViews, 1);
			}
			if (fcVa) {
				D3D12_CPU_DESCRIPTOR_HANDLE rtvs[kTargets];
				for (int i = 0; i < kTargets; ++i) {
					rtvs[i] = Rtv(i);
				}
				const auto dsv = Dsv(kDsvWrite);
				list->OMSetRenderTargets(kTargets, rtvs, FALSE, &dsv);
				list->OMSetStencilRef(static_cast<UINT>(Cfg().render.gbufferStencil & 0xFF));
				const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(depth.width), static_cast<float>(depth.height), 0, 1 };
				const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(depth.width), static_cast<LONG>(depth.height) };
				list->RSSetViewports(1, &vp);
				list->RSSetScissorRects(1, &sr);
				list->SetGraphicsRootSignature(g_root);
				ID3D12DescriptorHeap* heaps[] = { a_rec.heap };
				list->SetDescriptorHeaps(1, heaps);
				list->SetGraphicsRootConstantBufferView(render::kRootFrame, fcVa);
				list->SetGraphicsRootConstantBufferView(render::kRootErView, g_frameUsesEr ? RenderView::Slot(0) : fcVa);
				// t6 (the forward stages' depth test) is unused here but must be a valid table.
				a_rec.device->CopyDescriptorsSimple(1, a_rec.Cpu(a_rec.descUsed), a_rec.nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				list->SetGraphicsRootDescriptorTable(render::kRootDepth, a_rec.Gpu(a_rec.descUsed));
				++a_rec.descUsed;
				const auto fades = render::Fades(s);
				for (std::size_t i = 0; i < s.draws.size(); ++i) {
					const auto& d = s.draws[i];
					if (d.flags & proto::kDrawTranslucent) {
						++g_skippedTranslucent;  // Elden Ring's G-buffer is opaque; MGSV's translucent bits stay out
						continue;
					}
					render::BoundDraw b;
					const auto        r = render::BindDraw(a_rec, s, i, fades[i], b);
					if (r == BindResult::kFull) {
						break;
					}
					if (r != BindResult::kOk) {
						++(r == BindResult::kMissingMesh ? g_missing : g_bad);
						continue;
					}
					auto* pso = Pso(*b.mesh, d.flags, s.mirrored, Kind::kGbuffer);
					if (!pso) {
						++g_bad;
						continue;
					}
					list->SetPipelineState(pso);
					render::DrawMesh(list, d, *b.mesh);
					++drawn;
				}
			}
		}
		if (peek) {
			// Everything to COPY_SOURCE (from where our draws left it), copy the box, back.
			std::vector<D3D12_RESOURCE_BARRIER> in, out;
			for (int i = 0; i < kTargets; ++i) {
				Move(in, out, gb[i].resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			}
			Move(in, out, depth.resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->ResourceBarrier(static_cast<UINT>(in.size()), in.data());
			for (int i = 0; i < kTargets; ++i) {
				CopyPeek(list, i, gb[i].resource, 0);
			}
			CopyPeek(list, 5, depth.resource, 0);
			CopyPeek(list, 6, depth.resource, 1);
			list->ResourceBarrier(static_cast<UINT>(out.size()), out.data());
			g_peek.gbufRecorded = true;
			g_peek.pendingKind = 1;
		}
		if (!post.empty()) {
			list->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
		}
		if (drawing) {
			++g_frames;
			g_draws += drawn;
			if (g_frames == 1) {
				Log::Info("render: stage C: first frame into Elden Ring's G-buffer ({} draws, stencil {:#04x}, light set {})", drawn, Cfg().render.gbufferStencil & 0xFF,
					Cfg().render.gbufferLightSet >= 0 ? Cfg().render.gbufferLightSet : (g_learnedLightSet >= 0 ? g_learnedLightSet : kDefaultLightSet));
			}
		}
		return true;
	}

	bool RecordVelocity(render::Recording& a_rec, const RenderSnapshot* a_snap)
	{
		std::scoped_lock    lock(g_lock);
		RenderDepth::Target vel;
		RenderDepth::Target depth;
		if (!g_rtvHeap || !RenderDepth::Velocity(vel) || !RenderDepth::Current(depth)) {
			return false;
		}
		const bool drawing = Cfg().render.velocity && a_snap && a_snap->draw && a_snap->camera.valid && a_snap->havePrev;
		const bool peek = g_peek.wanted && g_peek.gbufRecorded && !g_peek.velRecorded && g_readback && g_peek.pendingKind == 0;
		if (!drawing && !peek) {
			return false;
		}
		EnsureRtv(kVelRtv, vel);
		EnsureDsv(kDsvRead, depth.resource);
		D3D12_RESOURCE_STATES stencilState = depth.state;
		RenderDepth::StencilPlane(stencilState);
		// A read-only DSV needs DEPTH_READ; shader-read bits Elden Ring left on may stay.
		auto readState = [](D3D12_RESOURCE_STATES a_s) {
			if (a_s & D3D12_RESOURCE_STATE_DEPTH_WRITE) {
				return D3D12_RESOURCE_STATE_DEPTH_READ;
			}
			return static_cast<D3D12_RESOURCE_STATES>(a_s | D3D12_RESOURCE_STATE_DEPTH_READ);
		};
		auto*                               list = a_rec.list;
		std::vector<D3D12_RESOURCE_BARRIER> pre, post;
		Move(pre, post, vel.resource, vel.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
		Move(pre, post, depth.resource, depth.state, readState(depth.state), 0);
		Move(pre, post, depth.resource, stencilState, readState(stencilState), 1);
		if (!pre.empty()) {
			list->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
		}
		std::uint32_t drawn = 0;
		if (drawing) {
			const auto&            s = *a_snap;
			render::FrameConstants fc{};
			render::FillFrame(fc, s, depth.width, depth.height, true);
			fc.misc[3] = g_frameUsesEr ? 1.0f : 0.0f;  // the constants copied for this frame's G-buffer pass are still there
			const auto fcVa = a_rec.Upload(&fc, sizeof(fc));
			if (fcVa) {
				const auto rtv = Rtv(kVelRtv);
				const auto dsv = Dsv(kDsvRead);
				list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
				const D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(depth.width), static_cast<float>(depth.height), 0, 1 };
				const D3D12_RECT     sr{ 0, 0, static_cast<LONG>(depth.width), static_cast<LONG>(depth.height) };
				list->RSSetViewports(1, &vp);
				list->RSSetScissorRects(1, &sr);
				list->SetGraphicsRootSignature(g_root);
				ID3D12DescriptorHeap* heaps[] = { a_rec.heap };
				list->SetDescriptorHeaps(1, heaps);
				list->SetGraphicsRootConstantBufferView(render::kRootFrame, fcVa);
				list->SetGraphicsRootConstantBufferView(render::kRootErView, g_frameUsesEr ? RenderView::Slot(0) : fcVa);
				a_rec.device->CopyDescriptorsSimple(1, a_rec.Cpu(a_rec.descUsed), a_rec.nullSrv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
				list->SetGraphicsRootDescriptorTable(render::kRootDepth, a_rec.Gpu(a_rec.descUsed));
				++a_rec.descUsed;
				const auto fades = render::Fades(s);
				for (std::size_t i = 0; i < s.draws.size(); ++i) {
					const auto& d = s.draws[i];
					if (d.flags & proto::kDrawTranslucent) {
						continue;
					}
					render::BoundDraw b;
					const auto        r = render::BindDraw(a_rec, s, i, fades[i], b);
					if (r == BindResult::kFull) {
						break;
					}
					if (r != BindResult::kOk) {
						continue;
					}
					auto* pso = Pso(*b.mesh, d.flags, s.mirrored, Kind::kVelocity);
					if (!pso) {
						continue;
					}
					list->SetPipelineState(pso);
					render::DrawMesh(list, d, *b.mesh);
					++drawn;
				}
			}
		}
		if (peek) {
			std::vector<D3D12_RESOURCE_BARRIER> in, out;
			Move(in, out, vel.resource, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			list->ResourceBarrier(static_cast<UINT>(in.size()), in.data());
			CopyPeek(list, 7, vel.resource, 0);
			list->ResourceBarrier(static_cast<UINT>(out.size()), out.data());
			g_peek.velRecorded = true;
			g_peek.pendingKind = 2;
		}
		if (!post.empty()) {
			list->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
		}
		if (drawing) {
			++g_velFrames;
			g_velDraws += drawn;
		}
		return true;
	}

	void SetSubmitFence(std::uint64_t a_fence)
	{
		std::scoped_lock lock(g_lock);
		if (g_peek.pendingKind == 1) {
			g_peek.gbufFence = a_fence;
		} else if (g_peek.pendingKind == 2) {
			g_peek.velFence = a_fence;
		}
		g_peek.pendingKind = 0;
	}

	void OnFence(std::uint64_t a_completed)
	{
		std::scoped_lock lock(g_lock);
		if (!g_peek.wanted || !g_peek.gbufRecorded || !g_peek.gbufFence || a_completed < g_peek.gbufFence) {
			return;
		}
		// Wait a few frames for the velocity part (it comes later in the frame, if at all).
		const bool velDone = g_peek.velRecorded && g_peek.velFence && a_completed >= g_peek.velFence;
		if (!velDone && a_completed < g_peek.gbufFence + 12) {
			return;
		}
		DecodePeek();
		g_peek.wanted = false;
	}

	void Reset()
	{
		RenderShadow::Reset();
		std::scoped_lock lock(g_lock);
		for (auto& r : g_rtvFor) {
			r = nullptr;
		}
		for (auto& d : g_dsvFor) {
			d = nullptr;
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		const auto&      rc = Cfg().render;
		return std::format("stage C: {} frames with Elden Ring's view constants, {} with our camera\n  {}\n  {}\n  {}\n  ", g_erFrames, g_ownFrames, RenderView::Describe(),
				   RenderShadow::Describe(), RenderBind::Describe()) +
		       std::format("{} frames into the G-buffer ({} draws), {} velocity frames ({} draws), {} missing mesh, {} bad, {} translucent skipped, {} pipelines ({} failed); "
						   "stencil {:#04x} light set {}{} ao {} gloss x{}+{} metal x{} velocity {} ({}, {}) near fade {}..{} / {}..{}{}",
			g_frames.load(), g_draws.load(), g_velFrames.load(), g_velDraws.load(), g_missing.load(), g_bad.load(), g_skippedTranslucent.load(), g_psos.size(), g_badPsos.size(),
			rc.gbufferStencil & 0xFF, rc.gbufferLightSet >= 0 ? rc.gbufferLightSet : (g_learnedLightSet >= 0 ? g_learnedLightSet : kDefaultLightSet),
			rc.gbufferLightSet >= 0 ? " (fixed)" : g_learnedLightSet >= 0 ? " (learned from the Tarnished)" : " (default; not learned yet)", rc.gbufferAo, rc.glossScale, rc.glossBias, rc.metalScale, rc.velocity ? "on" : "off", rc.velocityScale[0],
			rc.velocityScale[1], rc.nearFade[0], rc.nearFade[1], rc.nearFadeOther[0], rc.nearFadeOther[1], g_lastWhy.empty() ? "" : "; last refusal: " + g_lastWhy);
	}

	std::string Command(std::string_view a_args)
	{
		std::istringstream in{ std::string(a_args) };
		std::string        verb;
		in >> verb;
		if (verb.empty() || verb == "status") {
			return Describe() + "\n" + g_peekResult;
		}
		if (verb == "peek") {
			int x = -1, y = -1, n = 8;
			in >> x >> y >> n;
			std::scoped_lock lock(g_lock);
			if (!g_readback) {
				return "no readback buffer";
			}
			RenderDepth::Target depth;
			if (!RenderDepth::Current(depth)) {
				return "scene depth unknown";
			}
			g_peek = {};
			g_peek.x = x >= 0 ? x : static_cast<int>(depth.width / 2);
			g_peek.y = y >= 0 ? y : static_cast<int>(depth.height / 2);
			g_peek.n = std::clamp(n, 1, kPeekMax);
			g_peek.wanted = true;
			g_peekResult = "(peek pending)";
			return std::format("peek requested at ({}, {}) box {}: `render gbuf` shows it in a moment (and the log)", g_peek.x, g_peek.y, g_peek.n);
		}
		if (verb == "result") {
			return g_peekResult;
		}
		return "usage: render gbuf [status] | render gbuf peek [x y [n]] | render gbuf result";
	}
}
