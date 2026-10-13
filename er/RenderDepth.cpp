#include "RenderDepth.h"

#include "Hook.h"
#include "Log.h"
#include "RenderBind.h"
#include "RenderProbe.h"
#include "Retire.h"

#include <MinHook.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace phantom::er::RenderDepth
{
	namespace
	{
		using d3d::ComPtr;

		// vtable indices (mingw-w64 d3d12.h, computed with offsetof on the C vtables)
		constexpr std::size_t kDevCreateDsv = 21;
		constexpr std::size_t kListReset = 10;
		constexpr std::size_t kListResourceBarrier = 26;
		constexpr std::size_t kListOmSetRenderTargets = 46;
		constexpr std::size_t kListClearDsv = 47;
		constexpr std::size_t kListClearRtv = 48;

		using CreateDsvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
		using ResetFn = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
		using BarrierFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
		using OmSetFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
		using ClearDsvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
		using ClearRtvFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT*, UINT, const D3D12_RECT*);

		CreateDsvFn g_createDsv{ nullptr };
		ResetFn     g_reset{ nullptr };
		BarrierFn   g_barrier{ nullptr };
		OmSetFn     g_omSet{ nullptr };
		ClearDsvFn  g_clearDsv{ nullptr };
		ClearRtvFn  g_clearRtv{ nullptr };

		struct View
		{
			ID3D12Resource* resource{ nullptr };  // not owned: ER's
			DXGI_FORMAT     resFormat{ DXGI_FORMAT_UNKNOWN };
			UINT            width{ 0 }, height{ 0 };
			bool            denySrv{ false };
		};
		struct Usage
		{
			std::uint32_t clears{ 0 }, binds{ 0 };
			std::uint32_t fullClears{ 0 };  // without rects: all of it
			float         clearDepth{ 0 };
		};

		// The resources whose states we follow: [0] scene depth (plane 0), [1] scene colour, [2..6] the
		// G-buffer MRTs, [7] velocity, [8] the scene depth's stencil plane (subresource 1), [9..12] shadow
		// atlases (R16 depth targets rendered without colour: the sun's cascades and others).
		constexpr int kDepth = 0, kColor = 1, kGb0 = 2, kVel = 7, kStencil = 8, kAtlas0 = 9, kAtlases = 4, kSlots = kAtlas0 + kAtlases;
		using States = std::array<std::int64_t, kSlots>;  // -1 = no barrier on it in that list

		// Elden Ring 1.17.1's G-buffer (render probe, live 2026-10-12): normal, metal/rough/translucency/AO,
		// albedo + light-set index, two zero targets for opaque materials, then the scene depth.
		constexpr DXGI_FORMAT kGbufferFormats[kGbufferTargets] = { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
			DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT };

		std::atomic<bool>                                  g_installed{ false };
		ID3D12GraphicsCommandList*                         g_ours{ nullptr };  // the renderer's own list: not Elden Ring's passes
		UINT                                               g_rtvInc{ 32 };
		std::mutex                                         g_lock;
		std::unordered_map<SIZE_T, View>                   g_views;   // DSV descriptor -> resource
		std::unordered_map<SIZE_T, Usage>                  g_usage;   // this frame
		std::array<std::atomic<ID3D12Resource*>, kSlots>   g_watch{};
		ComPtr<ID3D12Resource>                             g_ref[kSlots];  // our references while we use them
		Target                                             g_target[kSlots];
		bool                                               g_known[kSlots]{};
		std::unordered_map<ID3D12CommandList*, States>     g_listState;
		std::unordered_map<ID3D12CommandList*, std::uint8_t> g_kinds;  // ListKind bits
		std::vector<ID3D12Resource*>                       g_backBuffers;
		std::unordered_map<ID3D12Resource*, std::uint32_t> g_colorBound;  // RGBA8 full-size targets rendered with the scene depth
		std::unordered_set<ID3D12Resource*>                g_colorCleared;
		UINT                                               g_bbW{ 0 }, g_bbH{ 0 };
		std::uint64_t                                      g_frames{ 0 }, g_dsvCreates{ 0 }, g_switches{ 0 };
		Retire<ComPtr<ID3D12Resource>>                     g_retired;          // old watched targets, until our lists are done
		std::uint64_t                                      g_submitted{ 0 };   // the renderer's last submission fence
		std::uint64_t                                      g_retiredTotal{ 0 }, g_atlasDrops{ 0 };
		std::uint32_t                                      g_atlasLogLines{ 0 };  // atlas acquisitions logged at info so far
		constexpr std::uint32_t                            kAtlasLogLines = 8;
		std::uint32_t                                      g_generation{ 0 };
		std::uint64_t                                      g_atlasBound[kAtlases]{};  // frame each atlas slot was last bound
		std::string                                        g_seen;

		// This frame's G-buffer / velocity binds (the candidates EndFrame adopts).
		struct Bound
		{
			ID3D12Resource* resource{ nullptr };
			DXGI_FORMAT     rtvFormat{ DXGI_FORMAT_UNKNOWN };
		};
		Bound         g_gbFrame[kGbufferTargets]{};
		Bound         g_velFrame{};
		std::uint32_t g_gbBinds{ 0 }, g_velBinds{ 0 };
		std::uint64_t g_gbLastFrame{ 0 }, g_velLastFrame{ 0 };
		std::string   g_fiveRtSeen;  // formats of other 5-target binds this frame (why stage C found no G-buffer)
		std::uint64_t g_gbFramesTotal{ 0 };

		DXGI_FORMAT SrvFormat(DXGI_FORMAT a_res)
		{
			switch (a_res) {
			case DXGI_FORMAT_R32G8X24_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
			case DXGI_FORMAT_R32_TYPELESS:
			case DXGI_FORMAT_D32_FLOAT:
				return DXGI_FORMAT_R32_FLOAT;
			case DXGI_FORMAT_R24G8_TYPELESS:
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			case DXGI_FORMAT_R16_TYPELESS:
			case DXGI_FORMAT_D16_UNORM:
				return DXGI_FORMAT_R16_UNORM;
			default:
				return DXGI_FORMAT_UNKNOWN;
			}
		}

		// A fully typed depth resource cannot be viewed as R32_FLOAT etc.; only typeless ones can.
		bool Typeless(DXGI_FORMAT a_f)
		{
			return a_f == DXGI_FORMAT_R32G8X24_TYPELESS || a_f == DXGI_FORMAT_R32_TYPELESS || a_f == DXGI_FORMAT_R24G8_TYPELESS || a_f == DXGI_FORMAT_R16_TYPELESS;
		}

		bool Rgba8(DXGI_FORMAT a_f)
		{
			return a_f == DXGI_FORMAT_R8G8B8A8_UNORM || a_f == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || a_f == DXGI_FORMAT_B8G8R8A8_UNORM ||
			       a_f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || a_f == DXGI_FORMAT_R8G8B8A8_TYPELESS || a_f == DXGI_FORMAT_B8G8R8A8_TYPELESS;
		}

		void STDMETHODCALLTYPE CreateDsvDetour(ID3D12Device* a_self, ID3D12Resource* a_res, const D3D12_DEPTH_STENCIL_VIEW_DESC* a_desc, D3D12_CPU_DESCRIPTOR_HANDLE a_h)
		{
			g_createDsv(a_self, a_res, a_desc, a_h);
			if (!a_res) {
				return;
			}
			const auto       d = a_res->GetDesc();
			std::scoped_lock lock(g_lock);
			auto&            v = g_views[a_h.ptr];
			v.resource = a_res;
			v.resFormat = d.Format;
			v.width = static_cast<UINT>(d.Width);
			v.height = d.Height;
			v.denySrv = (d.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0;
			++g_dsvCreates;
		}

		HRESULT STDMETHODCALLTYPE ResetDetour(ID3D12GraphicsCommandList* a_self, ID3D12CommandAllocator* a_alloc, ID3D12PipelineState* a_pso)
		{
			{
				std::scoped_lock lock(g_lock);
				g_listState.erase(a_self);
				g_kinds.erase(a_self);
			}
			RenderProbe::OnReset(a_self, a_pso);
			RenderBind::OnReset(a_self);
			return g_reset(a_self, a_alloc, a_pso);
		}

		void STDMETHODCALLTYPE BarrierDetour(ID3D12GraphicsCommandList* a_self, UINT a_n, const D3D12_RESOURCE_BARRIER* a_b)
		{
			ID3D12Resource* watched[kSlots];
			bool            any = false;
			for (int w = 0; w < kSlots; ++w) {
				watched[w] = g_watch[w].load(std::memory_order_relaxed);
				any |= watched[w] != nullptr;
			}
			if (any && a_self != g_ours) {
				for (UINT i = 0; i < a_n; ++i) {
					const auto& b = a_b[i];
					if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION || !b.Transition.pResource) {
						continue;
					}
					const auto sub = b.Transition.Subresource;
					for (int w = 0; w < kSlots; ++w) {
						if (w == kStencil || b.Transition.pResource != watched[w]) {
							continue;
						}
						// The depth resource has two planes: subresource 0 = depth, 1 = stencil.
						const bool plane0 = sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || sub == 0;
						const bool plane1 = w == kDepth && (sub == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES || sub == 1);
						if (!plane0 && !plane1) {
							continue;
						}
						std::scoped_lock lock(g_lock);
						auto [it, fresh] = g_listState.try_emplace(a_self);
						if (fresh) {
							it->second.fill(-1);
						}
						if (plane0) {
							it->second[w] = b.Transition.StateAfter;
						}
						if (plane1) {
							it->second[kStencil] = b.Transition.StateAfter;
						}
					}
				}
			}
			g_barrier(a_self, a_n, a_b);
		}

		void Watch(int a_which, ID3D12Resource* a_res, const Target& a_t);

		void STDMETHODCALLTYPE OmSetDetour(ID3D12GraphicsCommandList* a_self, UINT a_n, const D3D12_CPU_DESCRIPTOR_HANDLE* a_rtvs, BOOL a_single,
			const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv)
		{
			if (a_self != g_ours) {
				// Up to 5 colour targets (the composite and the world pass bind one, the G-buffer five).
				ID3D12Resource* rt[kGbufferTargets]{};
				DXGI_FORMAT     fmt[kGbufferTargets]{};
				UINT            w[kGbufferTargets]{}, h[kGbufferTargets]{};
				const UINT      n = a_rtvs ? std::min<UINT>(a_n, kGbufferTargets) : 0;
				bool            haveAll = n > 0;
				for (UINT i = 0; i < n; ++i) {
					const SIZE_T handle = a_single ? a_rtvs[0].ptr + std::size_t(i) * g_rtvInc : a_rtvs[i].ptr;
					haveAll &= RenderProbe::RtvInfo(handle, rt[i], fmt[i], w[i], h[i]);
				}
				std::scoped_lock lock(g_lock);
				ID3D12Resource*  ds = nullptr;
				if (a_dsv) {
					++g_usage[a_dsv->ptr].binds;
					if (const auto it = g_views.find(a_dsv->ptr); it != g_views.end()) {
						ds = it->second.resource;
					}
				}
				const auto& depth = g_target[kDepth];
				const bool  sceneDepth = ds && ds == depth.resource;
				// Which pass the list is in, for the binding watch (per-view constants).
				RenderBind::Pass pass = RenderBind::Pass::kOther;
				if (a_n == 0 && ds && a_dsv) {
					if (const auto it = g_views.find(a_dsv->ptr); it != g_views.end() && it->second.resFormat == DXGI_FORMAT_R16_TYPELESS && it->second.width >= 1024) {
						pass = RenderBind::Pass::kShadow;
						bool known = false;
						int  free = -1;
						for (int i = 0; i < kAtlases; ++i) {
							if (g_target[kAtlas0 + i].resource == ds) {
								known = true;
								g_atlasBound[i] = g_frames;
							}
							if (!g_target[kAtlas0 + i].resource && free < 0) {
								free = i;
							}
						}
						if (!known && free >= 0) {
							Target t;
							t.resource = ds;
							t.format = it->second.resFormat;
							t.width = it->second.width;
							t.height = it->second.height;
							Watch(kAtlas0 + free, ds, t);
							g_atlasBound[free] = g_frames;
							// Local-light atlases come and go every few seconds all session (bound, idle 60 frames, bound again):
							// the first few are worth a line, the rest are debug.
							if (++g_atlasLogLines <= kAtlasLogLines) {
								Log::Info("render: Elden Ring shadow atlas {} = {} {}x{} (R16){}", free, static_cast<void*>(ds), t.width, t.height,
									g_atlasLogLines == kAtlasLogLines ? " -- further atlas comings and goings are logged at debug level" : "");
							} else {
								Log::Debug("render: Elden Ring shadow atlas {} = {} {}x{} (R16)", free, static_cast<void*>(ds), t.width, t.height);
							}
						}
					}
				}
				if (n >= 1 && haveAll) {
					if (std::find(g_backBuffers.begin(), g_backBuffers.end(), rt[0]) != g_backBuffers.end()) {
						g_kinds[a_self] |= kListComposite;
					} else if (a_n == 1 && sceneDepth && Rgba8(fmt[0]) && w[0] == g_bbW && h[0] == g_bbH) {
						++g_colorBound[rt[0]];
					}
					if (a_n == 1 && sceneDepth && fmt[0] == DXGI_FORMAT_R16G16_FLOAT && w[0] == depth.width && h[0] == depth.height) {
						g_kinds[a_self] |= kListVelocity;
						g_velFrame = { rt[0], fmt[0] };
						++g_velBinds;
					}
					if (a_n == kGbufferTargets) {
						bool match = sceneDepth;
						for (int i = 0; i < kGbufferTargets; ++i) {
							match = match && fmt[i] == kGbufferFormats[i] && w[i] == depth.width && h[i] == depth.height;
						}
						if (match) {
							pass = RenderBind::Pass::kGbuffer;
							g_kinds[a_self] |= kListGbuffer;
							for (int i = 0; i < kGbufferTargets; ++i) {
								g_gbFrame[i] = { rt[i], fmt[i] };
							}
							++g_gbBinds;
						} else if (g_fiveRtSeen.size() < 200) {
							auto s = std::format("[{}/{}/{}/{}/{} {}x{}{}]", static_cast<int>(fmt[0]), static_cast<int>(fmt[1]), static_cast<int>(fmt[2]), static_cast<int>(fmt[3]),
								static_cast<int>(fmt[4]), w[0], h[0], sceneDepth ? " scene depth" : "");
							if (g_fiveRtSeen.find(s) == std::string::npos) {
								g_fiveRtSeen += s;
							}
						}
					}
				}
				RenderBind::OnTargets(a_self, pass, a_dsv ? a_dsv->ptr : 0, ds);
			}
			RenderProbe::OnSetTargets(a_self, a_n, a_rtvs, a_single, a_dsv);
			g_omSet(a_self, a_n, a_rtvs, a_single, a_dsv);
		}

		void STDMETHODCALLTYPE ClearDsvDetour(ID3D12GraphicsCommandList* a_self, D3D12_CPU_DESCRIPTOR_HANDLE a_h, D3D12_CLEAR_FLAGS a_flags, FLOAT a_depth,
			UINT8 a_stencil, UINT a_n, const D3D12_RECT* a_rects)
		{
			if ((a_flags & D3D12_CLEAR_FLAG_DEPTH) && a_self != g_ours) {
				std::scoped_lock lock(g_lock);
				auto&            u = g_usage[a_h.ptr];
				++u.clears;
				u.fullClears += a_n == 0 ? 1 : 0;
				u.clearDepth = a_depth;
			}
			g_clearDsv(a_self, a_h, a_flags, a_depth, a_stencil, a_n, a_rects);
		}

		void STDMETHODCALLTYPE ClearRtvDetour(ID3D12GraphicsCommandList* a_self, D3D12_CPU_DESCRIPTOR_HANDLE a_h, const FLOAT* a_c, UINT a_n, const D3D12_RECT* a_r)
		{
			if (a_self != g_ours) {
				ID3D12Resource* rt = nullptr;
				DXGI_FORMAT     fmt = DXGI_FORMAT_UNKNOWN;
				UINT            w = 0, h = 0;
				if (RenderProbe::RtvInfo(a_h.ptr, rt, fmt, w, h)) {
					std::scoped_lock lock(g_lock);
					g_colorCleared.insert(rt);
				}
				RenderProbe::OnClearRtv(a_self);
			}
			g_clearRtv(a_self, a_h, a_c, a_n, a_r);
		}

		void* Slot(void* a_object, std::size_t a_index) { return (*reinterpret_cast<void***>(a_object))[a_index]; }

		std::string StateName(std::int64_t a_s)
		{
			if (a_s < 0) {
				return "unknown";
			}
			std::string out;
			auto        add = [&](D3D12_RESOURCE_STATES a_bit, const char* a_name) {
                if ((a_s & a_bit) == a_bit && a_bit) {
                    out += (out.empty() ? "" : "|") + std::string(a_name);
                }
			};
			add(D3D12_RESOURCE_STATE_RENDER_TARGET, "RT");
			add(D3D12_RESOURCE_STATE_DEPTH_WRITE, "DEPTH_WRITE");
			add(D3D12_RESOURCE_STATE_DEPTH_READ, "DEPTH_READ");
			add(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, "NPSR");
			add(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, "PSR");
			add(D3D12_RESOURCE_STATE_COPY_SOURCE, "COPY_SRC");
			add(D3D12_RESOURCE_STATE_COPY_DEST, "COPY_DST");
			add(D3D12_RESOURCE_STATE_UNORDERED_ACCESS, "UAV");
			return out.empty() ? std::string("COMMON") : out;
		}

		// Locked. a_which's resource changes (null: let it go): drop what we knew about the old one.
		void Watch(int a_which, ID3D12Resource* a_res, const Target& a_t)
		{
			if (g_ref[a_which] && g_ref[a_which].Get() != a_res) {
				// A list of ours submitted before now (or being submitted on another thread) may still use the old one on
				// the GPU, and nothing else pins it (ER recreated it: graphics/shadow quality, no ResizeBuffers): keep it
				// until a submission after this one has completed.
				g_retired.Push(std::move(g_ref[a_which]), g_submitted + 1);
				++g_retiredTotal;
			}
			g_target[a_which] = a_t;
			g_target[a_which].generation = ++g_generation;
			g_ref[a_which] = a_res;  // keeps it alive while our lists may still use it
			g_known[a_which] = false;
			for (auto& [list, st] : g_listState) {
				st[a_which] = -1;
			}
			g_watch[a_which].store(a_res);
			++g_switches;
		}

		Target Describe(ID3D12Resource* a_res, DXGI_FORMAT a_rtvFormat)
		{
			const auto d = a_res->GetDesc();
			Target     t;
			t.resource = a_res;
			t.format = d.Format;
			t.rtvFormat = a_rtvFormat;
			t.width = static_cast<UINT>(d.Width);
			t.height = d.Height;
			return t;
		}
	}

	bool Install(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list)
	{
		if (g_installed.load()) {
			return true;
		}
		g_ours = a_list;
		g_rtvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		struct H
		{
			const char* name;
			void*       target;
			void*       detour;
			void**      original;
		};
		const H hooks[] = {
			{ "ID3D12Device::CreateDepthStencilView", Slot(a_device, kDevCreateDsv), reinterpret_cast<void*>(&CreateDsvDetour), reinterpret_cast<void**>(&g_createDsv) },
			{ "ID3D12GraphicsCommandList::Reset", Slot(a_list, kListReset), reinterpret_cast<void*>(&ResetDetour), reinterpret_cast<void**>(&g_reset) },
			{ "ID3D12GraphicsCommandList::ResourceBarrier", Slot(a_list, kListResourceBarrier), reinterpret_cast<void*>(&BarrierDetour), reinterpret_cast<void**>(&g_barrier) },
			{ "ID3D12GraphicsCommandList::OMSetRenderTargets", Slot(a_list, kListOmSetRenderTargets), reinterpret_cast<void*>(&OmSetDetour), reinterpret_cast<void**>(&g_omSet) },
			{ "ID3D12GraphicsCommandList::ClearDepthStencilView", Slot(a_list, kListClearDsv), reinterpret_cast<void*>(&ClearDsvDetour), reinterpret_cast<void**>(&g_clearDsv) },
			{ "ID3D12GraphicsCommandList::ClearRenderTargetView", Slot(a_list, kListClearRtv), reinterpret_cast<void*>(&ClearRtvDetour), reinterpret_cast<void**>(&g_clearRtv) },
		};
		std::vector<void*> made;
		bool               ok = true;
		for (const auto& h : hooks) {
			if (!Hooks::Create(h.name, h.target, h.detour, h.original)) {
				ok = false;
				break;
			}
			made.push_back(h.target);
		}
		for (void* t : made) {
			if (ok && MH_EnableHook(t) != MH_OK) {
				ok = false;
			}
		}
		if (!ok) {
			for (void* t : made) {
				MH_DisableHook(t);
				MH_RemoveHook(t);
			}
			Log::Error("render: frame watch hooks failed; Snake is drawn without Elden Ring's depth, over its HUD");
			return false;
		}
		g_installed = true;
		Log::Info("render: frame watch hooks live (DSV creation, clears, binds, barriers)");
		return true;
	}

	void SetBackBuffers(ID3D12Resource* const* a_buffers, UINT a_count)
	{
		std::scoped_lock lock(g_lock);
		g_backBuffers.assign(a_buffers, a_buffers + a_count);
	}

	void OnExecute(UINT a_count, ID3D12CommandList* const* a_lists)
	{
		bool any = false;
		for (const auto& w : g_watch) {
			any |= w.load(std::memory_order_relaxed) != nullptr;
		}
		if (!any) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (g_listState.empty()) {
			return;
		}
		for (UINT i = 0; i < a_count; ++i) {
			if (const auto it = g_listState.find(a_lists[i]); it != g_listState.end()) {
				for (int w = 0; w < kSlots; ++w) {
					if (it->second[w] >= 0) {
						g_target[w].state = static_cast<D3D12_RESOURCE_STATES>(it->second[w]);
						g_known[w] = true;
					}
				}
			}
		}
	}

	int CompositeIndex(UINT a_count, ID3D12CommandList* const* a_lists)
	{
		std::scoped_lock lock(g_lock);
		for (UINT i = 0; i < a_count; ++i) {
			if (const auto it = g_kinds.find(a_lists[i]); it != g_kinds.end() && (it->second & kListComposite)) {
				return static_cast<int>(i);
			}
		}
		return -1;
	}

	int LastIndex(ListKind a_kind, UINT a_count, ID3D12CommandList* const* a_lists)
	{
		std::scoped_lock lock(g_lock);
		if (g_kinds.empty()) {
			return -1;
		}
		for (int i = static_cast<int>(a_count) - 1; i >= 0; --i) {
			if (const auto it = g_kinds.find(a_lists[i]); it != g_kinds.end() && (it->second & a_kind)) {
				return i;
			}
		}
		return -1;
	}

	void EndFrame(UINT a_bbWidth, UINT a_bbHeight)
	{
		if (!g_installed.load() || a_bbHeight == 0) {
			return;
		}
		std::scoped_lock lock(g_lock);
		++g_frames;
		g_bbW = a_bbWidth;
		g_bbH = a_bbHeight;

		// Scene depth: the most-bound DSV cleared this frame with the back buffer's aspect, >= half its height.
		const float  aspect = static_cast<float>(a_bbWidth) / static_cast<float>(a_bbHeight);
		const View*  best = nullptr;
		const Usage* bestUse = nullptr;
		for (const auto& [h, u] : g_usage) {
			const auto it = g_views.find(h);
			if (it == g_views.end()) {
				continue;
			}
			const auto& v = it->second;
			if (u.clears) {
				// Log each kind of cleared depth target once (cascades come and go frame to frame).
				const auto item = std::format("[{}x{} fmt {}{} clear {:.1f}]", v.width, v.height, static_cast<int>(v.resFormat), v.denySrv ? " deny-SRV" : "", u.clearDepth);
				if (g_seen.find(item) == std::string::npos) {
					g_seen += item;
					Log::Info("render: Elden Ring clears depth target {} every frame ({} DSVs known)", item, g_views.size());
				}
			}
			if (!u.clears || v.height * 2 < a_bbHeight || std::abs(static_cast<float>(v.width) / static_cast<float>(v.height) - aspect) > 0.05f || v.denySrv ||
				SrvFormat(v.resFormat) == DXGI_FORMAT_UNKNOWN) {
				continue;
			}
			if (!best || u.binds > bestUse->binds) {
				best = &v;
				bestUse = &u;
			}
		}
		if (best && best->resource != g_target[kDepth].resource) {
			Target t;
			t.resource = best->resource;
			t.format = best->resFormat;
			t.srvFormat = SrvFormat(best->resFormat);
			t.width = best->width;
			t.height = best->height;
			t.clearDepth = bestUse->clearDepth;
			Watch(kDepth, best->resource, t);
			Watch(kStencil, best->resource, t);
			Log::Info("render: Elden Ring scene depth = {} {}x{} format {} ({}), cleared to {:.1f} ({}), bound {} times/frame{}", static_cast<void*>(best->resource),
				best->width, best->height, static_cast<int>(best->resFormat), d3d::Name(best->resFormat), bestUse->clearDepth,
				bestUse->clearDepth == 0.0f ? "reversed Z" : "standard Z", bestUse->binds, Typeless(best->resFormat) ? "" : " (typed: no SRV possible)");
		}

		// Scene colour: the full-size RGBA8 target rendered with the scene depth that is NOT cleared each
		// frame (the HUD target is cleared to transparent; the tone-mapped scene is overwritten instead).
		ID3D12Resource* color = nullptr;
		std::uint32_t   colorBinds = 0;
		for (const auto& [res, binds] : g_colorBound) {
			if (!g_colorCleared.contains(res) && binds > colorBinds) {
				color = res;
				colorBinds = binds;
			}
		}
		if (color && color != g_target[kColor].resource) {
			const auto t = Describe(color, DXGI_FORMAT_UNKNOWN);
			Watch(kColor, color, t);
			Log::Info("render: Elden Ring scene colour (post-tonemap, before the HUD) = {} {}x{} format {} ({}), {} world-pass binds/frame; {} cleared RGBA8 targets",
				static_cast<void*>(color), t.width, t.height, static_cast<int>(t.format), d3d::Name(t.format), colorBinds, g_colorCleared.size());
		}

		// G-buffer: the 5 MRTs bound with the scene depth this frame.
		if (g_gbBinds) {
			g_gbLastFrame = g_frames;
			++g_gbFramesTotal;
			bool changed = false;
			for (int i = 0; i < kGbufferTargets; ++i) {
				changed |= g_gbFrame[i].resource != g_target[kGb0 + i].resource;
			}
			if (changed) {
				std::string list;
				for (int i = 0; i < kGbufferTargets; ++i) {
					Watch(kGb0 + i, g_gbFrame[i].resource, Describe(g_gbFrame[i].resource, g_gbFrame[i].rtvFormat));
					list += std::format(" {}={}", i, d3d::Name(g_gbFrame[i].rtvFormat));
				}
				Log::Info("render: Elden Ring G-buffer = 5 targets{} {}x{} with the scene depth, {} binds/frame (stage C target)", list, g_target[kGb0].width,
					g_target[kGb0].height, g_gbBinds);
			}
		}
		if (g_velBinds) {
			g_velLastFrame = g_frames;
			if (g_velFrame.resource != g_target[kVel].resource) {
				Watch(kVel, g_velFrame.resource, Describe(g_velFrame.resource, g_velFrame.rtvFormat));
				Log::Info("render: Elden Ring velocity target = {} {}x{} {} ({} binds/frame)", static_cast<void*>(g_velFrame.resource), g_target[kVel].width,
					g_target[kVel].height, d3d::Name(g_velFrame.rtvFormat), g_velBinds);
			}
		}
		// Shadow atlases Elden Ring stopped binding (recreated, or this area doesn't use them): free the slot.
		for (int i = 0; i < kAtlases; ++i) {
			if (g_target[kAtlas0 + i].resource && g_frames - g_atlasBound[i] > kAtlasIdleFrames) {
				if (g_atlasLogLines < kAtlasLogLines) {
					Log::Info("render: Elden Ring shadow atlas {} ({}) not bound for {} frames -- let go", i, static_cast<void*>(g_target[kAtlas0 + i].resource),
						kAtlasIdleFrames);
				} else {
					Log::Debug("render: Elden Ring shadow atlas {} ({}) not bound for {} frames -- let go", i, static_cast<void*>(g_target[kAtlas0 + i].resource),
						kAtlasIdleFrames);
				}
				Watch(kAtlas0 + i, nullptr, Target{});
				++g_atlasDrops;
			}
		}
		g_gbBinds = g_velBinds = 0;
		g_usage.clear();
		g_colorBound.clear();
		g_colorCleared.clear();
		if (!g_fiveRtSeen.empty() && g_gbLastFrame + 1 < g_frames) {
			static std::string s_logged;
			if (s_logged != g_fiveRtSeen) {
				s_logged = g_fiveRtSeen;
				Log::Warn("render: 5-target binds that are not the G-buffer stage C expects: {}", g_fiveRtSeen);
			}
		}
		g_fiveRtSeen.clear();
	}

	bool Current(Target& a_out)
	{
		std::scoped_lock lock(g_lock);
		if (!g_target[kDepth].resource || !g_known[kDepth] || !Typeless(g_target[kDepth].format)) {
			return false;
		}
		a_out = g_target[kDepth];
		return true;
	}

	bool SceneColor(Target& a_out)
	{
		std::scoped_lock lock(g_lock);
		if (!g_target[kColor].resource || !g_known[kColor]) {
			return false;
		}
		a_out = g_target[kColor];
		return true;
	}

	bool Gbuffer(Target (&a_out)[kGbufferTargets], std::string* a_why)
	{
		std::scoped_lock lock(g_lock);
		auto             fail = [&](std::string a_text) {
            if (a_why) {
                *a_why = std::move(a_text);
            }
            return false;
		};
		if (!g_target[kDepth].resource || !g_known[kDepth]) {
			return fail("scene depth unknown");
		}
		if (g_target[kDepth].format != DXGI_FORMAT_R32G8X24_TYPELESS) {
			return fail(std::format("scene depth is {}, not R32G8X24_TYPELESS", d3d::Name(g_target[kDepth].format)));
		}
		if (g_gbLastFrame == 0 || g_gbLastFrame + 2 < g_frames) {
			return fail(g_gbLastFrame == 0 ? "no G-buffer bind seen yet (5 MRTs R10G10B10A2/RGBA8/RGBA8_SRGB/R8G8/RGBA16F with the scene depth)"
			                               : std::format("no G-buffer bind for {} frames", g_frames - g_gbLastFrame));
		}
		for (int i = 0; i < kGbufferTargets; ++i) {
			const auto& t = g_target[kGb0 + i];
			if (!t.resource || !g_known[kGb0 + i]) {
				return fail(std::format("G-buffer target {} state unknown", i));
			}
			if (t.width != g_target[kDepth].width || t.height != g_target[kDepth].height) {
				return fail(std::format("G-buffer target {} is {}x{}, the depth {}x{}", i, t.width, t.height, g_target[kDepth].width, g_target[kDepth].height));
			}
			a_out[i] = t;
		}
		return true;
	}

	bool Velocity(Target& a_out)
	{
		std::scoped_lock lock(g_lock);
		if (!g_target[kVel].resource || !g_known[kVel] || g_velLastFrame + 2 < g_frames) {
			return false;
		}
		a_out = g_target[kVel];
		return true;
	}

	ID3D12Resource* DsvResource(SIZE_T a_handle, std::uint32_t& a_w, std::uint32_t& a_h, int& a_format)
	{
		std::scoped_lock lock(g_lock);
		const auto       it = g_views.find(a_handle);
		if (it == g_views.end()) {
			return nullptr;
		}
		a_w = it->second.width;
		a_h = it->second.height;
		a_format = static_cast<int>(it->second.resFormat);
		return it->second.resource;
	}

	void SetSubmitFence(std::uint64_t a_fence)
	{
		std::scoped_lock lock(g_lock);
		g_submitted = std::max(g_submitted, a_fence);
	}

	void OnFence(std::uint64_t a_completed)
	{
		std::scoped_lock lock(g_lock);
		g_retired.Collect(a_completed);
	}

	std::uint32_t AtlasGeneration(ID3D12Resource* a_res)
	{
		std::scoped_lock lock(g_lock);
		for (int i = 0; i < kAtlases; ++i) {
			if (a_res && g_target[kAtlas0 + i].resource == a_res) {
				return g_target[kAtlas0 + i].generation;
			}
		}
		return 0;
	}

	bool FullyClearedThisFrame(ID3D12Resource* a_res)
	{
		std::scoped_lock lock(g_lock);
		for (const auto& [h, u] : g_usage) {
			if (u.fullClears) {
				if (const auto it = g_views.find(h); it != g_views.end() && it->second.resource == a_res) {
					return true;
				}
			}
		}
		return false;
	}

	void Reset()
	{
		std::scoped_lock lock(g_lock);
		g_retired.Clear();  // the renderer waited for the GPU first (OnResizeBefore)
		for (auto& f : g_atlasBound) {
			f = 0;
		}
		for (int w = 0; w < kSlots; ++w) {
			g_watch[w].store(nullptr);
			g_ref[w].Reset();
			g_target[w] = {};
			g_known[w] = false;
		}
		for (auto& b : g_gbFrame) {
			b = {};
		}
		g_velFrame = {};
		g_gbLastFrame = g_velLastFrame = 0;
		g_listState.clear();
		g_kinds.clear();
		g_backBuffers.clear();
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		const auto&      d = g_target[kDepth];
		const auto&      c = g_target[kColor];
		std::string      s = std::format("frame watch: hooks {}, {} DSVs known ({} created), {} switches\n  scene depth {} {}x{} fmt {} state {} (stencil plane {})\n  scene colour {} {}x{} fmt {} state {}",
				 g_installed.load() ? "live" : "off", g_views.size(), g_dsvCreates, g_switches, static_cast<void*>(d.resource), d.width, d.height, static_cast<int>(d.format),
				 g_known[kDepth] ? StateName(d.state) : "unknown", g_known[kStencil] ? StateName(g_target[kStencil].state) : "unknown", static_cast<void*>(c.resource), c.width,
				 c.height, static_cast<int>(c.format), g_known[kColor] ? StateName(c.state) : "unknown");
		s += std::format("\n  G-buffer: seen {} frames, last {} frames ago:", g_gbFramesTotal, g_gbLastFrame ? g_frames - g_gbLastFrame : 0);
		for (int i = 0; i < kGbufferTargets; ++i) {
			const auto& t = g_target[kGb0 + i];
			s += std::format(" [{} {} {}]", static_cast<void*>(t.resource), d3d::Name(t.rtvFormat), g_known[kGb0 + i] ? StateName(t.state) : "unknown");
		}
		const auto& v = g_target[kVel];
		s += std::format("\n  velocity {} {} {}x{} state {}", static_cast<void*>(v.resource), d3d::Name(v.rtvFormat), v.width, v.height,
			g_known[kVel] ? StateName(v.state) : "unknown");
		return s;
	}

	bool Atlas(ID3D12Resource* a_res, Target& a_out)
	{
		std::scoped_lock lock(g_lock);
		for (int i = 0; i < kAtlases; ++i) {
			if (g_target[kAtlas0 + i].resource == a_res && a_res) {
				if (!g_known[kAtlas0 + i]) {
					return false;
				}
				a_out = g_target[kAtlas0 + i];
				return true;
			}
		}
		return false;
	}

	bool StencilPlane(D3D12_RESOURCE_STATES& a_state)
	{
		std::scoped_lock lock(g_lock);
		if (!g_known[kStencil]) {
			return false;
		}
		a_state = g_target[kStencil].state;
		return true;
	}
}
