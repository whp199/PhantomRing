#include "RenderProbe.h"

#include "Hook.h"
#include "Log.h"
#include "RenderDepth.h"
#include "RenderPso.h"

#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace phantom::er::RenderProbe
{
	namespace
	{
		constexpr std::size_t kDevCreateRtv = 20;
		constexpr std::size_t kListDrawInstanced = 12;
		constexpr std::size_t kListDrawIndexedInstanced = 13;
		constexpr std::size_t kListDispatch = 14;
		constexpr std::size_t kListCopyTextureRegion = 16;
		constexpr std::size_t kListCopyResource = 17;
		constexpr std::size_t kListResolveSubresource = 19;
		constexpr std::size_t kListExecuteIndirect = 59;
		constexpr std::size_t kListBeginRenderPass = 68;
		constexpr std::size_t kListOmSetStencilRef = 24;
		constexpr std::size_t kListSetPipelineState = 25;
		constexpr std::size_t kListSetGraphicsRootSignature = 30;
		constexpr int         kMaxRootParams = 40;

		using CreateRtvFn = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
		using DrawFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
		using DrawIndexedFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
		using DispatchFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
		using CopyRegionFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT, const D3D12_TEXTURE_COPY_LOCATION*,
			const D3D12_BOX*);
		using CopyResFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
		using ResolveFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
		using IndirectFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
		using BeginPassFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*,
			D3D12_RENDER_PASS_FLAGS);
		using StencilRefFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT);
		using SetPsoFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
		using RootSigFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);

		CreateRtvFn   g_createRtv{ nullptr };
		DrawFn        g_draw{ nullptr };
		DrawIndexedFn g_drawIndexed{ nullptr };
		DispatchFn    g_dispatch{ nullptr };
		CopyRegionFn  g_copyRegion{ nullptr };
		CopyResFn     g_copyRes{ nullptr };
		ResolveFn     g_resolve{ nullptr };
		IndirectFn    g_indirect{ nullptr };
		BeginPassFn   g_beginPass{ nullptr };
		StencilRefFn  g_stencilRef{ nullptr };
		SetPsoFn      g_setPso{ nullptr };
		RootSigFn     g_rootSig{ nullptr };

		struct Rtv
		{
			ID3D12Resource* resource{ nullptr };
			DXGI_FORMAT     format{ DXGI_FORMAT_UNKNOWN };
			UINT            width{ 0 }, height{ 0 };
		};

		struct Pass
		{
			std::vector<SIZE_T> rtvs;
			SIZE_T              dsv{ 0 };
			std::uint32_t       draws{ 0 }, indexed{ 0 }, indirect{ 0 }, dispatches{ 0 }, clears{ 0 };
			std::vector<std::string> copies;  // texture copies/resolves in this pass: "src->dst" (WxH/format@resource)
			// pipeline -> draws/dispatches under it in this pass (names the shaders: RenderPso)
			std::vector<std::pair<const void*, std::uint32_t>> psos;
			std::vector<std::uint32_t>                         stencilRefs;
			// Bindings at the pass's first and last draw (shadow cascades / G-buffer: where their per-view
			// constants come from): viewport, root signature, descriptor tables as heap indices.
			struct Binding
			{
				D3D12_VIEWPORT vp{};
				const void*    root{ nullptr };
				std::int64_t   table[kMaxRootParams];
				Binding() { std::fill(std::begin(table), std::end(table), -1); }
			};
			Binding first, last;
			bool    haveFirst{ false };
			void Used(const void* a_pso)
			{
				for (auto& [p, n] : psos) {
					if (p == a_pso) {
						++n;
						return;
					}
				}
				psos.emplace_back(a_pso, 1);
			}
		};

		struct ListRec
		{
			const void*       pso{ nullptr };
			std::uint32_t     stencilRef{ 0 };
			Pass::Binding     bind;
			std::uint64_t     heapStart{ 0 };  // the CBV/SRV/UAV heap's GPU start
			UINT              heapInc{ 32 };
			std::vector<Pass> passes;
			Pass& Cur()
			{
				if (passes.empty()) {
					passes.emplace_back();
				}
				return passes.back();
			}
		};

		struct Exec
		{
			ID3D12CommandQueue*  queue;
			std::vector<ListRec> lists;
		};

		std::atomic<bool>                                g_installed{ false };
		std::atomic<int>                                 g_phase{ 0 };  // 0 idle, 1 lists, 2 lists + timeline, 3 dump requested
		ID3D12GraphicsCommandList*                       g_ours{ nullptr };
		ID3D12GraphicsCommandList*                       g_list{ nullptr };
		UINT                                             g_rtvInc{ 32 };
		UINT                                             g_srvInc{ 32 };
		bool                                             g_counters{ false };
		std::mutex                                       g_lock;
		std::unordered_map<SIZE_T, Rtv>                  g_rtvs;
		std::unordered_map<ID3D12CommandList*, ListRec>  g_lists;
		std::vector<Exec>                                g_timeline;
		std::uint64_t                                    g_probes{ 0 };
		bool                                             g_shaders{ false };  // this probe names each pass's shaders

		bool Recording(ID3D12GraphicsCommandList* a_list) { return g_phase.load(std::memory_order_relaxed) != 0 && a_list != g_ours; }

		void STDMETHODCALLTYPE CreateRtvDetour(ID3D12Device* a_self, ID3D12Resource* a_res, const D3D12_RENDER_TARGET_VIEW_DESC* a_desc, D3D12_CPU_DESCRIPTOR_HANDLE a_h)
		{
			g_createRtv(a_self, a_res, a_desc, a_h);
			if (a_res) {
				const auto d = a_res->GetDesc();
				std::scoped_lock lock(g_lock);
				g_rtvs[a_h.ptr] = { a_res, a_desc ? a_desc->Format : d.Format, static_cast<UINT>(d.Width), d.Height };
			}
		}

		template <class F>
		void Count(ID3D12GraphicsCommandList* a_list, F&& a_f, bool a_work = false)
		{
			if (Recording(a_list)) {
				std::scoped_lock lock(g_lock);
				auto&            l = g_lists[a_list];
				auto&            p = l.Cur();
				a_f(p);
				if (a_work) {
					p.Used(l.pso);
					if (!p.haveFirst) {
						p.first = l.bind;
						p.haveFirst = true;
					}
					p.last = l.bind;
					if (std::find(p.stencilRefs.begin(), p.stencilRefs.end(), l.stencilRef) == p.stencilRefs.end() && p.stencilRefs.size() < 8) {
						p.stencilRefs.push_back(l.stencilRef);
					}
				}
			}
		}

		void STDMETHODCALLTYPE DrawDetour(ID3D12GraphicsCommandList* a_self, UINT a, UINT b, UINT c, UINT d)
		{
			Count(a_self, [](Pass& p) { ++p.draws; }, true);
			g_draw(a_self, a, b, c, d);
		}
		void STDMETHODCALLTYPE DrawIndexedDetour(ID3D12GraphicsCommandList* a_self, UINT a, UINT b, UINT c, INT d, UINT e)
		{
			Count(a_self, [](Pass& p) { ++p.indexed; }, true);
			g_drawIndexed(a_self, a, b, c, d, e);
		}
		void STDMETHODCALLTYPE DispatchDetour(ID3D12GraphicsCommandList* a_self, UINT a, UINT b, UINT c)
		{
			Count(a_self, [](Pass& p) { ++p.dispatches; }, true);
			g_dispatch(a_self, a, b, c);
		}
		std::string Res(ID3D12Resource* a_r)
		{
			if (!a_r) {
				return "null";
			}
			const auto d = a_r->GetDesc();
			if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
				return std::format("buf{}@{}", d.Width, static_cast<void*>(a_r));
			}
			return std::format("{}x{}/{}@{}", d.Width, d.Height, static_cast<int>(d.Format), static_cast<void*>(a_r));
		}
		void NoteCopy(ID3D12GraphicsCommandList* a_list, const char* a_kind, ID3D12Resource* a_src, ID3D12Resource* a_dst)
		{
			if (!Recording(a_list)) {
				return;
			}
			auto text = std::format("{} {}->{}", a_kind, Res(a_src), Res(a_dst));
			std::scoped_lock lock(g_lock);
			auto&            p = g_lists[a_list].Cur();
			if (p.copies.size() < 12) {
				p.copies.push_back(std::move(text));
			}
		}
		void STDMETHODCALLTYPE CopyRegionDetour(ID3D12GraphicsCommandList* a_self, const D3D12_TEXTURE_COPY_LOCATION* a_dst, UINT x, UINT y, UINT z,
			const D3D12_TEXTURE_COPY_LOCATION* a_src, const D3D12_BOX* a_box)
		{
			NoteCopy(a_self, "region", a_src ? a_src->pResource : nullptr, a_dst ? a_dst->pResource : nullptr);
			g_copyRegion(a_self, a_dst, x, y, z, a_src, a_box);
		}
		void STDMETHODCALLTYPE CopyResDetour(ID3D12GraphicsCommandList* a_self, ID3D12Resource* a_dst, ID3D12Resource* a_src)
		{
			NoteCopy(a_self, "copy", a_src, a_dst);
			g_copyRes(a_self, a_dst, a_src);
		}
		void STDMETHODCALLTYPE ResolveDetour(ID3D12GraphicsCommandList* a_self, ID3D12Resource* a_dst, UINT a_ds, ID3D12Resource* a_src, UINT a_ss, DXGI_FORMAT a_f)
		{
			NoteCopy(a_self, "resolve", a_src, a_dst);
			g_resolve(a_self, a_dst, a_ds, a_src, a_ss, a_f);
		}
		void STDMETHODCALLTYPE IndirectDetour(ID3D12GraphicsCommandList* a_self, ID3D12CommandSignature* a_sig, UINT a_n, ID3D12Resource* a_args, UINT64 a_off,
			ID3D12Resource* a_cnt, UINT64 a_cntOff)
		{
			Count(a_self, [](Pass& p) { ++p.indirect; }, true);
			g_indirect(a_self, a_sig, a_n, a_args, a_off, a_cnt, a_cntOff);
		}
		void STDMETHODCALLTYPE StencilRefDetour(ID3D12GraphicsCommandList* a_self, UINT a_ref)
		{
			if (Recording(a_self)) {
				std::scoped_lock lock(g_lock);
				g_lists[a_self].stencilRef = a_ref;
			}
			g_stencilRef(a_self, a_ref);
		}
		void STDMETHODCALLTYPE RootSigDetour(ID3D12GraphicsCommandList* a_self, ID3D12RootSignature* a_root)
		{
			if (Recording(a_self)) {
				std::scoped_lock lock(g_lock);
				auto&            l = g_lists[a_self];
				l.bind.root = a_root;
				std::fill(std::begin(l.bind.table), std::end(l.bind.table), -1);
			}
			g_rootSig(a_self, a_root);
		}

		std::string Bindings(const Pass::Binding& a_b)
		{
			std::string s = std::format("vp {:.0f},{:.0f} {:.0f}x{:.0f} z{:.1f}-{:.1f} rs {}", a_b.vp.TopLeftX, a_b.vp.TopLeftY, a_b.vp.Width, a_b.vp.Height, a_b.vp.MinDepth,
				a_b.vp.MaxDepth, a_b.root);
			for (int i = 0; i < kMaxRootParams; ++i) {
				if (a_b.table[i] != -1) {
					s += std::format(" t{}={}", i, a_b.table[i]);
				}
			}
			return s;
		}

		void STDMETHODCALLTYPE SetPsoDetour(ID3D12GraphicsCommandList* a_self, ID3D12PipelineState* a_pso)
		{
			if (Recording(a_self)) {
				std::scoped_lock lock(g_lock);
				g_lists[a_self].pso = a_pso;
			}
			g_setPso(a_self, a_pso);
		}

		// "ps 0123456789abcdef x12" per pipeline of a pass (the dump file names of VKD3D_SHADER_DUMP_PATH).
		std::string Shaders(const Pass& a_p)
		{
			std::string s;
			int         shown = 0;
			auto        sorted = a_p.psos;
			std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
			for (const auto& [pso, n] : sorted) {
				if (++shown > 6) {
					s += std::format(" +{} more", sorted.size() - 6);
					break;
				}
				RenderPso::Info i;
				if (!RenderPso::Find(pso, i)) {
					s += std::format(" pso{}?x{}", pso, n);
				} else if (i.cs) {
					s += std::format(" cs {:016x} x{}", i.cs, n);
				} else {
					std::string masks;
					for (int r = 0; r < i.numRt && r < 8; ++r) {
						masks += std::format("{:x}", i.writeMask[r] & 0xF);
					}
					s += std::format(" ps {:016x}/vs {:016x} x{}{} z{}{} m{}{}", i.ps, i.vs, n, i.stencil ? std::format(" st w{:02x}p{}f{}", i.stencilWrite, i.stencilPass, i.stencilFunc) : "",
						i.depth ? static_cast<int>(i.depthFunc) : 0, i.depthWrite ? "w" : "r", masks.empty() ? "-" : masks, i.blend0 ? " blend" : "");
				}
			}
			if (!a_p.stencilRefs.empty()) {
				s += " ref";
				for (auto r : a_p.stencilRefs) {
					s += std::format(" {:#x}", r);
				}
			}
			return s;
		}
		void STDMETHODCALLTYPE BeginPassDetour(ID3D12GraphicsCommandList4* a_self, UINT a_n, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* a_rts,
			const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* a_ds, D3D12_RENDER_PASS_FLAGS a_flags)
		{
			auto* list = static_cast<ID3D12GraphicsCommandList*>(a_self);
			if (Recording(list)) {
				std::scoped_lock lock(g_lock);
				Pass p;
				for (UINT i = 0; i < a_n; ++i) {
					p.rtvs.push_back(a_rts[i].cpuDescriptor.ptr);
				}
				p.dsv = a_ds ? a_ds->cpuDescriptor.ptr : 0;
				g_lists[list].passes.push_back(std::move(p));
			}
			g_beginPass(a_self, a_n, a_rts, a_ds, a_flags);
		}

		void* Slot(void* a_object, std::size_t a_index) { return (*reinterpret_cast<void***>(a_object))[a_index]; }

		std::string Target(SIZE_T a_h, ID3D12Resource* const* a_bbs, UINT a_n)
		{
			const auto it = g_rtvs.find(a_h);
			if (it == g_rtvs.end()) {
				return std::format("rtv{:#x}?", a_h);
			}
			const auto& r = it->second;
			for (UINT i = 0; i < a_n; ++i) {
				if (a_bbs[i] == r.resource) {
					return std::format("BACKBUFFER{}", i);
				}
			}
			return std::format("{}x{}/{}@{}", r.width, r.height, static_cast<int>(r.format), static_cast<void*>(r.resource));
		}

		void Dump(ID3D12Resource* const* a_bbs, UINT a_n, ID3D12Resource* a_mainDepth)
		{
			std::scoped_lock lock(g_lock);
			std::size_t lists = 0, passes = 0;
			for (const auto& e : g_timeline) {
				lists += e.lists.size();
				for (const auto& l : e.lists) {
					passes += l.passes.size();
				}
			}
			Log::Info("render probe: one frame = {} ExecuteCommandLists batches, {} lists, {} passes (main depth {})", g_timeline.size(), lists, passes,
				static_cast<void*>(a_mainDepth));
			int ei = 0;
			for (const auto& e : g_timeline) {
				D3D12_COMMAND_QUEUE_DESC qd{};
				if (e.queue) {
					qd = e.queue->GetDesc();
				}
				Log::Info("render probe: ECL #{} queue {} type {} lists {}", ei++, static_cast<void*>(e.queue), static_cast<int>(qd.Type), e.lists.size());
				int li = 0;
				for (const auto& l : e.lists) {
					std::string line;
					for (const auto& p : l.passes) {
						if (!p.draws && !p.indexed && !p.indirect && !p.dispatches && !p.clears && p.copies.empty()) {
							continue;
						}
						std::string rts;
						for (auto h : p.rtvs) {
							rts += (rts.empty() ? "" : ",") + Target(h, a_bbs, a_n);
						}
						std::string ds;
						if (p.dsv) {
							std::uint32_t w = 0, h = 0;
							int           fmt = 0;
							auto*         res = RenderDepth::DsvResource(p.dsv, w, h, fmt);
							ds = res == a_mainDepth && res ? std::string(" DS=MAIN") : std::format(" DS={}x{}/{}", w, h, fmt);
						}
						line += std::format(" | [{}]{} d{} i{} x{} c{} k{}", rts, ds, p.draws, p.indexed, p.indirect, p.dispatches, p.clears);
						for (const auto& c : p.copies) {
							line += " (" + c + ")";
						}
						if (g_shaders) {
							line += " {" + Shaders(p) + " }";
							if (p.haveFirst) {
								line += " <" + Bindings(p.first) + " | last " + Bindings(p.last) + ">";
							}
						}
					}
					if (!line.empty()) {
						Log::Info("render probe:   list {}{}", li, line);
					}
					++li;
				}
			}
			g_timeline.clear();
			g_lists.clear();
		}
	}

	bool Install(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list)
	{
		if (g_installed.load()) {
			return true;
		}
		g_ours = a_list;
		g_list = a_list;
		g_rtvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
		g_srvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		void* target = Slot(a_device, kDevCreateRtv);
		if (!Hooks::Create("ID3D12Device::CreateRenderTargetView", target, reinterpret_cast<void*>(&CreateRtvDetour), reinterpret_cast<void**>(&g_createRtv)) ||
			MH_EnableHook(target) != MH_OK) {
			Log::Warn("render: RTV watch failed (render probe unavailable)");
			return false;
		}
		g_installed = true;
		return true;
	}

	namespace
	{
		// The per-draw counters cost a few ns per Elden Ring draw call: installed on the first probe only.
		bool InstallCounters()
		{
			if (g_counters) {
				return true;
			}
			struct H
			{
				const char* name;
				void*       target;
				void*       detour;
				void**      original;
			};
			auto* l = g_list;
			const H hooks[] = {
				{ "ID3D12GraphicsCommandList::DrawInstanced", Slot(l, kListDrawInstanced), reinterpret_cast<void*>(&DrawDetour), reinterpret_cast<void**>(&g_draw) },
				{ "ID3D12GraphicsCommandList::DrawIndexedInstanced", Slot(l, kListDrawIndexedInstanced), reinterpret_cast<void*>(&DrawIndexedDetour),
					reinterpret_cast<void**>(&g_drawIndexed) },
				{ "ID3D12GraphicsCommandList::Dispatch", Slot(l, kListDispatch), reinterpret_cast<void*>(&DispatchDetour), reinterpret_cast<void**>(&g_dispatch) },
				{ "ID3D12GraphicsCommandList::CopyTextureRegion", Slot(l, kListCopyTextureRegion), reinterpret_cast<void*>(&CopyRegionDetour),
					reinterpret_cast<void**>(&g_copyRegion) },
				{ "ID3D12GraphicsCommandList::CopyResource", Slot(l, kListCopyResource), reinterpret_cast<void*>(&CopyResDetour), reinterpret_cast<void**>(&g_copyRes) },
				{ "ID3D12GraphicsCommandList::ResolveSubresource", Slot(l, kListResolveSubresource), reinterpret_cast<void*>(&ResolveDetour),
					reinterpret_cast<void**>(&g_resolve) },
				{ "ID3D12GraphicsCommandList::ExecuteIndirect", Slot(l, kListExecuteIndirect), reinterpret_cast<void*>(&IndirectDetour), reinterpret_cast<void**>(&g_indirect) },
				{ "ID3D12GraphicsCommandList::OMSetStencilRef", Slot(l, kListOmSetStencilRef), reinterpret_cast<void*>(&StencilRefDetour), reinterpret_cast<void**>(&g_stencilRef) },
				{ "ID3D12GraphicsCommandList::SetPipelineState", Slot(l, kListSetPipelineState), reinterpret_cast<void*>(&SetPsoDetour), reinterpret_cast<void**>(&g_setPso) },
				{ "ID3D12GraphicsCommandList::SetGraphicsRootSignature", Slot(l, kListSetGraphicsRootSignature), reinterpret_cast<void*>(&RootSigDetour),
					reinterpret_cast<void**>(&g_rootSig) },
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
			d3d::ComPtr<ID3D12GraphicsCommandList4> l4;
			if (ok && SUCCEEDED(l->QueryInterface(__uuidof(ID3D12GraphicsCommandList4), reinterpret_cast<void**>(l4.GetAddressOf())))) {
				if (Hooks::Create("ID3D12GraphicsCommandList4::BeginRenderPass", Slot(l4.Get(), kListBeginRenderPass), reinterpret_cast<void*>(&BeginPassDetour),
						reinterpret_cast<void**>(&g_beginPass))) {
					made.push_back(Slot(l4.Get(), kListBeginRenderPass));
				}
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
				Log::Warn("render: frame probe counters failed");
				return false;
			}
			g_counters = true;
			return true;
		}
	}

	void Request(bool a_shaders)
	{
		if (g_installed.load() && g_phase.load() == 0 && InstallCounters()) {
			std::scoped_lock lock(g_lock);
			g_lists.clear();
			g_timeline.clear();
			g_shaders = a_shaders;
			g_phase = 1;
			++g_probes;
		}
	}

	void OnSetTargets(ID3D12GraphicsCommandList* a_list, UINT a_n, const D3D12_CPU_DESCRIPTOR_HANDLE* a_rtvs, BOOL a_single, const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv)
	{
		if (!Recording(a_list)) {
			return;
		}
		Pass p;
		for (UINT i = 0; i < a_n && a_rtvs; ++i) {
			D3D12_CPU_DESCRIPTOR_HANDLE h = a_single ? D3D12_CPU_DESCRIPTOR_HANDLE{ a_rtvs[0].ptr + i * g_rtvInc } : a_rtvs[i];
			p.rtvs.push_back(h.ptr);
		}
		p.dsv = a_dsv ? a_dsv->ptr : 0;
		std::scoped_lock lock(g_lock);
		g_lists[a_list].passes.push_back(std::move(p));
	}

	void OnViewports(ID3D12GraphicsCommandList* a_list, UINT a_n, const D3D12_VIEWPORT* a_vp)
	{
		if (Recording(a_list) && a_n && a_vp) {
			std::scoped_lock lock(g_lock);
			g_lists[a_list].bind.vp = a_vp[0];
		}
	}

	void OnHeaps(ID3D12GraphicsCommandList* a_list, UINT a_n, ID3D12DescriptorHeap* const* a_heaps)
	{
		if (!Recording(a_list)) {
			return;
		}
		for (UINT i = 0; i < a_n; ++i) {
			if (a_heaps[i] && a_heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
				std::scoped_lock lock(g_lock);
				auto&            l = g_lists[a_list];
				l.heapStart = a_heaps[i]->GetGPUDescriptorHandleForHeapStart().ptr;
				l.heapInc = g_srvInc;
			}
		}
	}

	void OnRootTable(ID3D12GraphicsCommandList* a_list, UINT a_param, D3D12_GPU_DESCRIPTOR_HANDLE a_h)
	{
		if (Recording(a_list) && a_param < kMaxRootParams) {
			std::scoped_lock lock(g_lock);
			auto&            l = g_lists[a_list];
			l.bind.table[a_param] = l.heapStart && a_h.ptr >= l.heapStart ? static_cast<std::int64_t>((a_h.ptr - l.heapStart) / l.heapInc) : -2;
		}
	}

	void OnClearRtv(ID3D12GraphicsCommandList* a_list)
	{
		Count(a_list, [](Pass& p) { ++p.clears; });
	}

	bool RtvInfo(SIZE_T a_handle, ID3D12Resource*& a_res, DXGI_FORMAT& a_format, UINT& a_w, UINT& a_h)
	{
		std::scoped_lock lock(g_lock);
		const auto       it = g_rtvs.find(a_handle);
		if (it == g_rtvs.end()) {
			return false;
		}
		a_res = it->second.resource;
		a_format = it->second.format;
		a_w = it->second.width;
		a_h = it->second.height;
		return true;
	}

	void OnReset(ID3D12GraphicsCommandList* a_list, ID3D12PipelineState* a_initial)
	{
		if (Recording(a_list)) {
			std::scoped_lock lock(g_lock);
			g_lists.erase(a_list);
			if (a_initial) {
				g_lists[a_list].pso = a_initial;
			}
		}
	}

	void OnExecute(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
	{
		if (g_phase.load(std::memory_order_relaxed) != 2) {
			return;
		}
		std::scoped_lock lock(g_lock);
		Exec e{ a_queue, {} };
		for (UINT i = 0; i < a_count; ++i) {
			if (a_lists[i] == g_ours) {
				continue;
			}
			const auto it = g_lists.find(a_lists[i]);
			e.lists.push_back(it != g_lists.end() ? it->second : ListRec{});
		}
		g_timeline.push_back(std::move(e));
	}

	void OnPresent(IDXGISwapChain*, ID3D12Resource* const* a_bbs, UINT a_count, ID3D12Resource* a_mainDepth, D3D12_CPU_DESCRIPTOR_HANDLE)
	{
		const int phase = g_phase.load();
		if (phase == 1) {
			g_phase = 2;  // lists recorded from now on belong to complete frames: collect the next one
			std::scoped_lock lock(g_lock);
			g_timeline.clear();
		} else if (phase == 2) {
			g_phase = 0;
			Dump(a_bbs, a_count, a_mainDepth);
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("probe: hooks {}, {} RTVs known, {} probes, phase {}", g_installed.load() ? "live" : "off", g_rtvs.size(), g_probes, g_phase.load());
	}
}
