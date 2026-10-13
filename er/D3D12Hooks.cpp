#include "D3D12Hooks.h"

#include "Hook.h"
#include "Log.h"

#include <MinHook.h>

#include <atomic>
#include <chrono>
#include <format>
#include <mutex>
#include <vector>

namespace phantom::er::D3D12Hooks
{
	namespace
	{
		using d3d::ComPtr;

		using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
		using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
		using ResizeFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
		using Resize1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
		using ColorSpaceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
		using CreateSwapFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
		using CreateSwapHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
			const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
		using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

		PresentFn        g_present{ nullptr };
		Present1Fn       g_present1{ nullptr };
		ResizeFn         g_resize{ nullptr };
		Resize1Fn        g_resize1{ nullptr };
		ColorSpaceFn     g_colorSpace{ nullptr };
		CreateSwapFn     g_createSwap{ nullptr };
		CreateSwapHwndFn g_createSwapHwnd{ nullptr };
		ExecuteFn        g_execute{ nullptr };

		Callbacks             g_cb;
		std::atomic<bool>     g_installed{ false };
		std::atomic<DXGI_COLOR_SPACE_TYPE> g_space{ DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709 };
		std::vector<void*>    g_targets;

		// swap chain -> presenting queue (Elden Ring has one swap chain; a few entries are plenty)
		struct Pair
		{
			IDXGISwapChain*                 swapchain;
			ComPtr<ID3D12CommandQueue>      queue;
			const char*                     how;
		};
		std::mutex                       g_lock;
		std::vector<Pair>                g_pairs;
		std::vector<IDXGISwapChain*>     g_unmatched;  // presenting swap chains whose queue is unknown
		std::vector<ID3D12CommandQueue*> g_tested;     // queues already checked against g_unmatched
		std::atomic<bool>                g_needQueue{ false };
		thread_local int                 t_inPresent = 0;

		ComPtr<ID3D12CommandQueue> DirectQueue(IUnknown* a_unknown)
		{
			ComPtr<ID3D12CommandQueue> q;
			if (a_unknown && SUCCEEDED(a_unknown->QueryInterface(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(q.GetAddressOf())))) {
				const auto desc = q->GetDesc();
				if (desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
					return q;
				}
			}
			return {};
		}

		void Remember(IDXGISwapChain* a_swapchain, ComPtr<ID3D12CommandQueue> a_queue, const char* a_how)
		{
			std::scoped_lock lock(g_lock);
			for (auto& p : g_pairs) {
				if (p.swapchain == a_swapchain) {
					if (p.queue.Get() != a_queue.Get()) {
						Log::Info("render: swap chain {} now presents on queue {} ({})", static_cast<void*>(a_swapchain), static_cast<void*>(a_queue.Get()), a_how);
						p.queue = a_queue;
						p.how = a_how;
					}
					return;
				}
			}
			g_pairs.push_back({ a_swapchain, a_queue, a_how });
			std::erase(g_unmatched, a_swapchain);
			g_needQueue.store(!g_unmatched.empty(), std::memory_order_release);
			Log::Info("render: swap chain {} presents on direct queue {} (found via {})", static_cast<void*>(a_swapchain), static_cast<void*>(a_queue.Get()), a_how);
		}

		bool Known(IDXGISwapChain* a_swapchain)
		{
			std::scoped_lock lock(g_lock);
			for (const auto& p : g_pairs) {
				if (p.swapchain == a_swapchain) {
					return true;
				}
			}
			return false;
		}

		// First Present of a swap chain whose creation we didn't see: ask it for its queue.
		void Discover(IDXGISwapChain* a_swapchain)
		{
			if (Known(a_swapchain)) {
				return;
			}
			ComPtr<IUnknown> dev;
			if (SUCCEEDED(a_swapchain->GetDevice(__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(dev.GetAddressOf())))) {
				if (auto q = DirectQueue(dev.Get())) {
					Remember(a_swapchain, q, "IDXGISwapChain::GetDevice");
					return;
				}
			}
			std::scoped_lock lock(g_lock);
			if (std::find(g_unmatched.begin(), g_unmatched.end(), a_swapchain) == g_unmatched.end()) {
				g_unmatched.push_back(a_swapchain);
				g_tested.clear();
				g_needQueue.store(true, std::memory_order_release);
				Log::Info("render: swap chain {} did not name its queue; matching ExecuteCommandLists queues against it", static_cast<void*>(a_swapchain));
			}
		}

		// hudhook's check_command_queue: is the queue pointer stored inside the swap-chain object?
		void MatchQueue(ID3D12CommandQueue* a_queue)
		{
			IDXGISwapChain* match = nullptr;
			{
				std::scoped_lock lock(g_lock);
				if (std::find(g_tested.begin(), g_tested.end(), a_queue) != g_tested.end()) {
					return;
				}
				g_tested.push_back(a_queue);
				for (auto* sc : g_unmatched) {
					void*  words[512]{};
					SIZE_T read = 0;
					::ReadProcessMemory(::GetCurrentProcess(), sc, words, sizeof(words), &read);
					for (std::size_t i = 0; i < read / sizeof(void*); ++i) {
						if (words[i] == a_queue) {
							match = sc;
							Log::Info("render: queue {} found at swap chain +{:#x}", static_cast<void*>(a_queue), i * sizeof(void*));
							break;
						}
					}
					if (match) {
						break;
					}
				}
			}
			if (match) {
				if (auto q = DirectQueue(a_queue)) {
					Remember(match, q, "ExecuteCommandLists + swap-chain scan");
				}
			}
		}

		HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* a_self, UINT a_sync, UINT a_flags)
		{
			if (t_inPresent++ == 0 && !(a_flags & DXGI_PRESENT_TEST)) {
				Discover(a_self);
				if (g_cb.present) {
					g_cb.present(a_self, a_flags);
				}
			}
			const HRESULT hr = g_present(a_self, a_sync, a_flags);
			--t_inPresent;
			return hr;
		}

		HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* a_self, UINT a_sync, UINT a_flags, const DXGI_PRESENT_PARAMETERS* a_params)
		{
			if (t_inPresent++ == 0 && !(a_flags & DXGI_PRESENT_TEST)) {
				Discover(a_self);
				if (g_cb.present) {
					g_cb.present(a_self, a_flags);
				}
			}
			const HRESULT hr = g_present1(a_self, a_sync, a_flags, a_params);
			--t_inPresent;
			return hr;
		}

		HRESULT STDMETHODCALLTYPE ResizeDetour(IDXGISwapChain* a_self, UINT a_count, UINT a_w, UINT a_h, DXGI_FORMAT a_format, UINT a_flags)
		{
			if (g_cb.resizeBefore) {
				g_cb.resizeBefore(a_self);
			}
			const HRESULT hr = g_resize(a_self, a_count, a_w, a_h, a_format, a_flags);
			Log::Info("render: ResizeBuffers({}, {}x{}, format {}) -> {}", a_count, a_w, a_h, static_cast<int>(a_format), d3d::Hr(hr));
			if (g_cb.resizeAfter) {
				g_cb.resizeAfter(a_self, hr);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE Resize1Detour(IDXGISwapChain3* a_self, UINT a_count, UINT a_w, UINT a_h, DXGI_FORMAT a_format, UINT a_flags,
			const UINT* a_nodeMask, IUnknown* const* a_queues)
		{
			if (g_cb.resizeBefore) {
				g_cb.resizeBefore(a_self);
			}
			const HRESULT hr = g_resize1(a_self, a_count, a_w, a_h, a_format, a_flags, a_nodeMask, a_queues);
			Log::Info("render: ResizeBuffers1({}, {}x{}, format {}) -> {}", a_count, a_w, a_h, static_cast<int>(a_format), d3d::Hr(hr));
			if (SUCCEEDED(hr) && a_queues && a_queues[0]) {
				if (auto q = DirectQueue(a_queues[0])) {
					Remember(a_self, q, "ResizeBuffers1");
				}
			}
			if (g_cb.resizeAfter) {
				g_cb.resizeAfter(a_self, hr);
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE ColorSpaceDetour(IDXGISwapChain3* a_self, DXGI_COLOR_SPACE_TYPE a_space)
		{
			const HRESULT hr = g_colorSpace(a_self, a_space);
			if (SUCCEEDED(hr) && g_space.exchange(a_space) != a_space) {
				Log::Info("render: swap chain colour space set to {}", static_cast<int>(a_space));
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateSwapDetour(IDXGIFactory* a_self, IUnknown* a_device, DXGI_SWAP_CHAIN_DESC* a_desc, IDXGISwapChain** a_out)
		{
			const HRESULT hr = g_createSwap(a_self, a_device, a_desc, a_out);
			if (SUCCEEDED(hr) && a_out && *a_out) {
				if (auto q = DirectQueue(a_device)) {
					Remember(*a_out, q, "CreateSwapChain");
				}
			}
			return hr;
		}

		HRESULT STDMETHODCALLTYPE CreateSwapHwndDetour(IDXGIFactory2* a_self, IUnknown* a_device, HWND a_hwnd, const DXGI_SWAP_CHAIN_DESC1* a_desc,
			const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* a_fs, IDXGIOutput* a_output, IDXGISwapChain1** a_out)
		{
			const HRESULT hr = g_createSwapHwnd(a_self, a_device, a_hwnd, a_desc, a_fs, a_output, a_out);
			if (SUCCEEDED(hr) && a_out && *a_out && a_desc) {
				Log::Info("render: game created a swap chain {}x{} format {} ({}) buffers {} flags {:#x}", a_desc->Width, a_desc->Height, static_cast<int>(a_desc->Format),
					d3d::Name(a_desc->Format), a_desc->BufferCount, a_desc->Flags);
				if (auto q = DirectQueue(a_device)) {
					Remember(*a_out, q, "CreateSwapChainForHwnd");
				}
			}
			return hr;
		}

		void STDMETHODCALLTYPE ExecuteDetour(ID3D12CommandQueue* a_self, UINT a_count, ID3D12CommandList* const* a_lists)
		{
			if (g_needQueue.load(std::memory_order_acquire)) {
				MatchQueue(a_self);
			}
			if (g_cb.execute && g_cb.execute(a_self, a_count, a_lists)) {
				return;
			}
			g_execute(a_self, a_count, a_lists);
		}

		void* Slot(void* a_object, std::size_t a_index) { return (*reinterpret_cast<void***>(a_object))[a_index]; }

		bool Detour(const char* a_name, void* a_target, void* a_detour, void** a_original)
		{
			if (!Hooks::Create(a_name, a_target, a_detour, a_original)) {
				return false;
			}
			g_targets.push_back(a_target);
			return true;
		}

		LRESULT CALLBACK DummyWndProc(HWND a_hwnd, UINT a_msg, WPARAM a_wp, LPARAM a_lp) { return ::DefWindowProcW(a_hwnd, a_msg, a_wp, a_lp); }
	}

	bool Install(const Callbacks& a_callbacks)
	{
		if (g_installed.load()) {
			return true;
		}
		g_cb = a_callbacks;
		const auto t0 = std::chrono::steady_clock::now();

		// The game imports both DLLs, so they are loaded; LoadLibrary just takes a reference.
		HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
		HMODULE dxgi = ::LoadLibraryW(L"dxgi.dll");
		auto createDevice = d3d12 ? reinterpret_cast<PFN_D3D12_CREATE_DEVICE>(reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12CreateDevice"))) : nullptr;
		using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
		auto createFactory = dxgi ? reinterpret_cast<CreateFactoryFn>(reinterpret_cast<void*>(::GetProcAddress(dxgi, "CreateDXGIFactory1"))) : nullptr;
		if (!createDevice || !createFactory) {
			Log::Error("render: d3d12.dll/dxgi.dll entry points missing; renderer off");
			return false;
		}

		WNDCLASSEXW wc{ sizeof(wc) };
		wc.lpfnWndProc = &DummyWndProc;
		wc.hInstance = ::GetModuleHandleW(nullptr);
		wc.lpszClassName = L"PhantomRingDummyD3D12";
		::RegisterClassExW(&wc);
		HWND hwnd = ::CreateWindowExW(0, wc.lpszClassName, L"PhantomRing", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

		ComPtr<IDXGIFactory2>      factory;
		ComPtr<ID3D12Device>       device;
		ComPtr<ID3D12CommandQueue> queue;
		ComPtr<IDXGISwapChain1>    swapchain;
		HRESULT                    hr = createFactory(__uuidof(IDXGIFactory2), reinterpret_cast<void**>(factory.GetAddressOf()));
		if (SUCCEEDED(hr)) {
			hr = createDevice(nullptr, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(device.GetAddressOf()));
		}
		if (SUCCEEDED(hr)) {
			D3D12_COMMAND_QUEUE_DESC qd{};
			qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
			hr = device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(queue.GetAddressOf()));
		}
		if (SUCCEEDED(hr) && hwnd) {
			DXGI_SWAP_CHAIN_DESC1 sd{};
			sd.Width = 64;
			sd.Height = 64;
			sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			sd.SampleDesc.Count = 1;
			sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
			sd.BufferCount = 2;
			sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
			hr = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &sd, nullptr, nullptr, swapchain.GetAddressOf());
		}
		if (FAILED(hr) || !swapchain) {
			Log::Error("render: dummy D3D12 objects failed ({}); renderer off", d3d::Hr(hr));
			swapchain.Reset();
			queue.Reset();
			device.Reset();
			factory.Reset();
			if (hwnd) {
				::DestroyWindow(hwnd);
			}
			return false;
		}

		bool ok = true;
		ok &= Detour("IDXGISwapChain::Present", Slot(swapchain.Get(), d3d::vt::kSwapPresent), reinterpret_cast<void*>(&PresentDetour), reinterpret_cast<void**>(&g_present));
		ok &= Detour("IDXGISwapChain1::Present1", Slot(swapchain.Get(), d3d::vt::kSwapPresent1), reinterpret_cast<void*>(&Present1Detour), reinterpret_cast<void**>(&g_present1));
		ok &= Detour("IDXGISwapChain::ResizeBuffers", Slot(swapchain.Get(), d3d::vt::kSwapResizeBuffers), reinterpret_cast<void*>(&ResizeDetour), reinterpret_cast<void**>(&g_resize));
		ok &= Detour("ID3D12CommandQueue::ExecuteCommandLists", Slot(queue.Get(), d3d::vt::kQueueExecuteCommandLists), reinterpret_cast<void*>(&ExecuteDetour),
			reinterpret_cast<void**>(&g_execute));
		// Optional ones: their absence only costs a fallback.
		ComPtr<IDXGISwapChain3> sc3;
		if (ok && SUCCEEDED(swapchain.As(&sc3))) {
			Detour("IDXGISwapChain3::ResizeBuffers1", Slot(sc3.Get(), d3d::vt::kSwapResizeBuffers1), reinterpret_cast<void*>(&Resize1Detour), reinterpret_cast<void**>(&g_resize1));
			Detour("IDXGISwapChain3::SetColorSpace1", Slot(sc3.Get(), d3d::vt::kSwapSetColorSpace1), reinterpret_cast<void*>(&ColorSpaceDetour), reinterpret_cast<void**>(&g_colorSpace));
		}
		if (ok) {
			Detour("IDXGIFactory::CreateSwapChain", Slot(factory.Get(), d3d::vt::kFactoryCreateSwapChain), reinterpret_cast<void*>(&CreateSwapDetour), reinterpret_cast<void**>(&g_createSwap));
			Detour("IDXGIFactory2::CreateSwapChainForHwnd", Slot(factory.Get(), d3d::vt::kFactoryCreateSwapChainForHwnd), reinterpret_cast<void*>(&CreateSwapHwndDetour),
				reinterpret_cast<void**>(&g_createSwapHwnd));
		}

		if (ok && g_cb.dummyDevice) {
			g_cb.dummyDevice(device.Get());
		}

		// Release the dummies before the detours go live, so none of their calls reach our code.
		sc3.Reset();
		swapchain.Reset();
		queue.Reset();
		device.Reset();
		factory.Reset();
		::DestroyWindow(hwnd);

		if (ok) {
			for (void* target : g_targets) {
				if (const auto status = MH_EnableHook(target); status != MH_OK) {
					Log::Error("render: enabling hook at {}: {}", target, MH_StatusToString(status));
					ok = false;
				}
			}
		}
		if (!ok) {
			for (void* target : g_targets) {
				MH_DisableHook(target);
				MH_RemoveHook(target);
			}
			g_targets.clear();
			Log::Error("render: D3D12 hooks failed; renderer off");
			return false;
		}
		const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		Log::Info("render: D3D12/DXGI hooks live ({} detours, {:.0f} ms)", g_targets.size(), ms);
		g_installed.store(true);
		return true;
	}

	bool Installed() { return g_installed.load(); }

	void ExecuteOriginal(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists)
	{
		if (a_count) {
			g_execute(a_queue, a_count, a_lists);
		}
	}

	ComPtr<ID3D12CommandQueue> PresentQueue(IDXGISwapChain* a_swapchain)
	{
		std::scoped_lock lock(g_lock);
		for (const auto& p : g_pairs) {
			if (p.swapchain == a_swapchain) {
				return p.queue;
			}
		}
		return {};
	}

	DXGI_COLOR_SPACE_TYPE ColorSpace() { return g_space.load(); }

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		std::string      s = std::format("hooks {} ({} detours), colour space {}", g_installed.load() ? "live" : "off", g_targets.size(), static_cast<int>(g_space.load()));
		for (const auto& p : g_pairs) {
			s += std::format(", swap chain {} -> queue {} ({})", static_cast<void*>(p.swapchain), static_cast<void*>(p.queue.Get()), p.how);
		}
		if (!g_unmatched.empty()) {
			s += std::format(", {} swap chain(s) without a queue yet", g_unmatched.size());
		}
		return s;
	}
}
