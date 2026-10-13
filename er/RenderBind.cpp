#include "RenderBind.h"

#include "Hook.h"
#include "Log.h"
#include "RenderProbe.h"

#include <MinHook.h>

#include <atomic>
#include <format>
#include <mutex>
#include <unordered_map>

namespace phantom::er::RenderBind
{
	namespace
	{
		constexpr std::size_t kListRsSetViewports = 21;
		constexpr std::size_t kListSetDescriptorHeaps = 28;
		constexpr std::size_t kListSetGraphicsRootDescriptorTable = 32;
		// Root parameter 1's table holds the view constants at its 5th descriptor (decompiled vertex shaders:
		// BindlessCBV[registers[1] + 4]).
		constexpr UINT kViewParam = 1;
		constexpr UINT kViewOffset = 4;

		using ViewportsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
		using HeapsFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
		using RootTableFn = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);

		ViewportsFn g_viewports{ nullptr };
		HeapsFn     g_heaps{ nullptr };
		RootTableFn g_rootTable{ nullptr };

		struct ListState
		{
			ID3D12DescriptorHeap* heap{ nullptr };
			Pass                  pass{ Pass::kOther };
			D3D12_VIEWPORT        vp{};
			SIZE_T                dsv{ 0 };
			ID3D12Resource*       depth{ nullptr };
			bool                  haveGbuffer{ false };
			View                  gbuffer;
			std::vector<View>     shadows;  // one per viewport
		};

		std::atomic<bool>                                  g_installed{ false };
		ID3D12GraphicsCommandList*                         g_ours{ nullptr };
		UINT                                               g_srvInc{ 32 };
		std::mutex                                         g_lock;
		std::unordered_map<ID3D12CommandList*, ListState>  g_lists;
		std::atomic<std::uint64_t>                         g_gbViews{ 0 }, g_shadowViews{ 0 }, g_collects{ 0 };

		bool SameViewport(const D3D12_VIEWPORT& a, const D3D12_VIEWPORT& b)
		{
			return a.TopLeftX == b.TopLeftX && a.TopLeftY == b.TopLeftY && a.Width == b.Width && a.Height == b.Height;
		}

		void STDMETHODCALLTYPE ViewportsDetour(ID3D12GraphicsCommandList* a_self, UINT a_n, const D3D12_VIEWPORT* a_vp)
		{
			if (a_self != g_ours && a_n && a_vp) {
				std::scoped_lock lock(g_lock);
				g_lists[a_self].vp = a_vp[0];
			}
			RenderProbe::OnViewports(a_self, a_n, a_vp);
			g_viewports(a_self, a_n, a_vp);
		}

		void STDMETHODCALLTYPE HeapsDetour(ID3D12GraphicsCommandList* a_self, UINT a_n, ID3D12DescriptorHeap* const* a_heaps)
		{
			if (a_self != g_ours) {
				for (UINT i = 0; i < a_n; ++i) {
					if (a_heaps[i] && a_heaps[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) {
						std::scoped_lock lock(g_lock);
						g_lists[a_self].heap = a_heaps[i];
					}
				}
			}
			RenderProbe::OnHeaps(a_self, a_n, a_heaps);
			g_heaps(a_self, a_n, a_heaps);
		}

		void STDMETHODCALLTYPE RootTableDetour(ID3D12GraphicsCommandList* a_self, UINT a_param, D3D12_GPU_DESCRIPTOR_HANDLE a_h)
		{
			if (a_param == kViewParam && a_self != g_ours) {
				std::scoped_lock lock(g_lock);
				auto&            l = g_lists[a_self];
				if (l.pass != Pass::kOther) {
					View v;
					v.cb.ptr = a_h.ptr + std::uint64_t(kViewOffset) * g_srvInc;
					v.vp = l.vp;
					v.dsv.ptr = l.dsv;
					v.depth = l.depth;
					if (l.pass == Pass::kGbuffer) {
						l.gbuffer = v;
						l.haveGbuffer = true;
					} else {
						bool found = false;
						for (auto& s : l.shadows) {
							if (SameViewport(s.vp, v.vp) && s.depth == v.depth) {
								s = v;
								found = true;
							}
						}
						if (!found && l.shadows.size() < 16) {
							l.shadows.push_back(v);
						}
					}
				}
			}
			RenderProbe::OnRootTable(a_self, a_param, a_h);
			g_rootTable(a_self, a_param, a_h);
		}

		void* Slot(void* a_object, std::size_t a_index) { return (*reinterpret_cast<void***>(a_object))[a_index]; }
	}

	bool Install(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_ours)
	{
		if (g_installed.load()) {
			return true;
		}
		g_ours = a_ours;
		g_srvInc = a_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		struct H
		{
			const char* name;
			void*       target;
			void*       detour;
			void**      original;
		};
		const H hooks[] = {
			{ "ID3D12GraphicsCommandList::RSSetViewports", Slot(a_ours, kListRsSetViewports), reinterpret_cast<void*>(&ViewportsDetour), reinterpret_cast<void**>(&g_viewports) },
			{ "ID3D12GraphicsCommandList::SetDescriptorHeaps", Slot(a_ours, kListSetDescriptorHeaps), reinterpret_cast<void*>(&HeapsDetour), reinterpret_cast<void**>(&g_heaps) },
			{ "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable", Slot(a_ours, kListSetGraphicsRootDescriptorTable), reinterpret_cast<void*>(&RootTableDetour),
				reinterpret_cast<void**>(&g_rootTable) },
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
			Log::Warn("render: binding watch hooks failed: stage C uses its own camera and Snake casts no shadow");
			return false;
		}
		g_installed = true;
		Log::Info("render: binding watch live (descriptor heaps, viewports, root parameter {} tables)", kViewParam);
		return true;
	}

	void OnTargets(ID3D12GraphicsCommandList* a_list, Pass a_pass, SIZE_T a_dsv, ID3D12Resource* a_depth)
	{
		if (!g_installed.load(std::memory_order_relaxed) || a_list == g_ours) {
			return;
		}
		std::scoped_lock lock(g_lock);
		auto&            l = g_lists[a_list];
		l.pass = a_pass;
		l.dsv = a_dsv;
		l.depth = a_depth;
	}

	void OnReset(ID3D12GraphicsCommandList* a_list)
	{
		if (!g_installed.load(std::memory_order_relaxed)) {
			return;
		}
		std::scoped_lock lock(g_lock);
		g_lists.erase(a_list);
	}

	bool Collect(UINT a_count, ID3D12CommandList* const* a_lists, View& a_gbuffer, std::vector<View>& a_shadows, ID3D12DescriptorHeap*& a_heap)
	{
		std::scoped_lock lock(g_lock);
		++g_collects;
		bool gb = false;
		a_shadows.clear();
		a_heap = nullptr;
		for (UINT i = 0; i < a_count; ++i) {
			const auto it = g_lists.find(a_lists[i]);
			if (it == g_lists.end()) {
				continue;
			}
			const auto& l = it->second;
			if (l.heap) {
				a_heap = l.heap;
			}
			if (l.haveGbuffer) {
				a_gbuffer = l.gbuffer;
				gb = true;
			}
			for (const auto& s : l.shadows) {
				bool found = false;
				for (auto& have : a_shadows) {
					if (SameViewport(have.vp, s.vp) && have.depth == s.depth) {
						have = s;
						found = true;
					}
				}
				if (!found) {
					a_shadows.push_back(s);
				}
			}
		}
		g_gbViews += gb ? 1 : 0;
		g_shadowViews += a_shadows.size();
		return gb && a_heap;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return std::format("binding watch: hooks {}, {} lists tracked, {} collects ({} with a G-buffer view, {} shadow views)", g_installed.load() ? "live" : "off",
			g_lists.size(), g_collects.load(), g_gbViews.load(), g_shadowViews.load());
	}
}
