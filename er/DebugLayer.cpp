#include "DebugLayer.h"

#include "Log.h"

#include <d3d12sdklayers.h>

#include <atomic>
#include <cstring>
#include <format>
#include <vector>

namespace phantom::er::DebugLayer
{
	// ---- pure ----

	const char* SeverityName(int a_severity)
	{
		switch (a_severity) {
		case D3D12_MESSAGE_SEVERITY_CORRUPTION: return "CORRUPTION";
		case D3D12_MESSAGE_SEVERITY_ERROR:      return "ERROR";
		case D3D12_MESSAGE_SEVERITY_WARNING:    return "WARNING";
		case D3D12_MESSAGE_SEVERITY_INFO:       return "INFO";
		case D3D12_MESSAGE_SEVERITY_MESSAGE:    return "MESSAGE";
		default:                                return "?";
		}
	}

	bool MessageFilter::Take(int a_severity, int a_id, bool* a_suppressedFirst)
	{
		if (a_suppressedFirst) {
			*a_suppressedFirst = false;
		}
		if (a_severity < 0 || a_severity > 4) {
			a_severity = D3D12_MESSAGE_SEVERITY_MESSAGE;
		}
		++m_counts[a_severity];
		if (a_severity > D3D12_MESSAGE_SEVERITY_WARNING) {
			return false;  // info and plain messages: counted only
		}
		const unsigned seen = ++m_perIdSeen[a_id];
		if (seen > static_cast<unsigned>(m_perId) || m_logged >= static_cast<std::uint64_t>(m_total)) {
			if (a_suppressedFirst && seen == static_cast<unsigned>(m_perId) + 1 && m_logged < static_cast<std::uint64_t>(m_total)) {
				*a_suppressedFirst = true;
			}
			++m_dropped;
			return false;
		}
		++m_logged;
		return true;
	}

	std::uint64_t MessageFilter::Count(int a_severity) const { return a_severity >= 0 && a_severity <= 4 ? m_counts[a_severity] : 0; }

	std::string MessageFilter::Summary() const
	{
		return std::format("{} corruption, {} errors, {} warnings, {} info ({} ids; {} logged, {} not logged)", m_counts[0], m_counts[1], m_counts[2],
			m_counts[3] + m_counts[4], m_perIdSeen.size(), m_logged, m_dropped);
	}

	// ---- live ----

	namespace
	{
		using d3d::ComPtr;
		using GetDebugInterfaceFn = HRESULT(WINAPI*)(REFIID, void**);

		std::atomic<bool> g_enabled{ false };
		bool              g_layer{ false };  // the debug layer itself (not only DRED)
		bool              g_gbv{ false };
		bool              g_dred{ false };
		std::string       g_enableResult{ "off" };

		std::recursive_mutex       g_lock;  // the filter, and the device objects below (recursive: a D3D12 call made under it may call back)
		MessageFilter              g_filter;
		ComPtr<ID3D12Device>       g_device;
		ComPtr<ID3D12InfoQueue>    g_queue;
		ComPtr<ID3D12InfoQueue1>   g_queue1;
		bool                       g_callback{ false };
		std::atomic<bool>          g_attached{ false };
		std::atomic<bool>          g_removedLogged{ false };
		std::string                g_attachResult{ "no device yet" };
		thread_local int           t_ours{ 0 };

		void Handle(D3D12_MESSAGE_CATEGORY a_category, D3D12_MESSAGE_SEVERITY a_severity, D3D12_MESSAGE_ID a_id, const char* a_text)
		{
			bool take = false;
			bool firstSuppressed = false;
			{
				std::scoped_lock lock(g_lock);
				take = g_filter.Take(static_cast<int>(a_severity), static_cast<int>(a_id), &firstSuppressed);
			}
			if (take) {
				Log::Warn("d3d12 {} #{} (category {}){}: {}", SeverityName(a_severity), static_cast<int>(a_id), static_cast<int>(a_category),
					t_ours ? " [during PhantomRing's work]" : "", a_text ? a_text : "");
			} else if (firstSuppressed) {
				Log::Info("d3d12: message #{} repeats; not logged again (`pr er \"render\"` shows the counts)", static_cast<int>(a_id));
			}
		}

		void __stdcall Callback(D3D12_MESSAGE_CATEGORY a_category, D3D12_MESSAGE_SEVERITY a_severity, D3D12_MESSAGE_ID a_id, const char* a_text, void*)
		{
			Handle(a_category, a_severity, a_id, a_text);
		}

		bool ProcessHasWindow()
		{
			struct Ctx
			{
				DWORD pid;
				HWND  found;
			} ctx{ ::GetCurrentProcessId(), nullptr };
			::EnumWindows(
				[](HWND a_hwnd, LPARAM a_lp) -> BOOL {
					auto* c = reinterpret_cast<Ctx*>(a_lp);
					DWORD pid = 0;
					::GetWindowThreadProcessId(a_hwnd, &pid);
					if (pid != c->pid) {
						return TRUE;
					}
					wchar_t cls[64]{};
					::GetClassNameW(a_hwnd, cls, 64);
					// user32's own helper windows, and ours.
					if (!std::wcscmp(cls, L"IME") || !std::wcscmp(cls, L"MSCTFIME UI") || !std::wcsncmp(cls, L"PhantomRing", 11)) {
						return TRUE;
					}
					c->found = a_hwnd;
					return FALSE;
				},
				reinterpret_cast<LPARAM>(&ctx));
			return ctx.found != nullptr;
		}

		std::string Name(const char* a_a, const wchar_t* a_w)
		{
			if (a_a && *a_a) {
				return a_a;
			}
			if (a_w && *a_w) {
				return Narrow(a_w);
			}
			return "unnamed";
		}

		void LogDred(ID3D12Device* a_device)
		{
			ComPtr<ID3D12DeviceRemovedExtendedData> dred;
			if (FAILED(a_device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData), reinterpret_cast<void**>(dred.GetAddressOf())))) {
				Log::Error("d3d12: no DRED data on the device");
				return;
			}
			D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
			if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs))) {
				int shown = 0;
				for (auto* n = crumbs.pHeadAutoBreadcrumbNode; n && shown < 16; n = n->pNext) {
					const UINT32 last = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
					if (last >= n->BreadcrumbCount) {
						continue;  // this list finished on the GPU
					}
					++shown;
					const int op = n->pCommandHistory && last < n->BreadcrumbCount ? static_cast<int>(n->pCommandHistory[last]) : -1;
					Log::Error("d3d12 DRED: list '{}' {} on queue '{}' {}: {} of {} operations done, stopped at op {} (D3D12_AUTO_BREADCRUMB_OP)",
						Name(n->pCommandListDebugNameA, n->pCommandListDebugNameW), static_cast<void*>(n->pCommandList),
						Name(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW), static_cast<void*>(n->pCommandQueue), last, n->BreadcrumbCount, op);
				}
				if (!shown) {
					Log::Error("d3d12 DRED: no unfinished command list in the breadcrumbs");
				}
			}
			D3D12_DRED_PAGE_FAULT_OUTPUT fault{};
			if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)) && fault.PageFaultVA) {
				Log::Error("d3d12 DRED: GPU page fault at 0x{:X}", fault.PageFaultVA);
				int k = 0;
				for (auto* a = fault.pHeadExistingAllocationNode; a && k < 8; a = a->pNext, ++k) {
					Log::Error("d3d12 DRED:   live allocation there: '{}' (type {})", Name(a->ObjectNameA, a->ObjectNameW), static_cast<int>(a->AllocationType));
				}
				k = 0;
				for (auto* a = fault.pHeadRecentFreedAllocationNode; a && k < 8; a = a->pNext, ++k) {
					Log::Error("d3d12 DRED:   recently freed there: '{}' (type {})", Name(a->ObjectNameA, a->ObjectNameW), static_cast<int>(a->AllocationType));
				}
			}
		}

		void DrainStored(ID3D12InfoQueue* a_queue)
		{
			const UINT64 n = a_queue->GetNumStoredMessages();
			std::vector<unsigned char> buf;
			for (UINT64 i = 0; i < n; ++i) {
				SIZE_T len = 0;
				if (FAILED(a_queue->GetMessage(i, nullptr, &len)) || len < sizeof(D3D12_MESSAGE)) {
					continue;
				}
				buf.resize(len);
				auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
				if (SUCCEEDED(a_queue->GetMessage(i, m, &len))) {
					Handle(m->Category, m->Severity, m->ID, m->pDescription);
				}
			}
			if (n) {
				a_queue->ClearStoredMessages();
			}
		}
	}

	OurScope::OurScope() { ++t_ours; }
	OurScope::~OurScope() { --t_ours; }

	bool Enable(bool a_gpuValidation)
	{
		if (g_enabled.load()) {
			return true;
		}
		if (ProcessHasWindow()) {
			g_enableResult = "not enabled: Elden Ring already had a window (its D3D12 device may exist; enabling now would remove it)";
			Log::Warn("render: [Render] debug_layer {}", g_enableResult);
			return false;
		}
		HMODULE d3d12 = ::LoadLibraryW(L"d3d12.dll");
		auto    getDebug = d3d12 ? reinterpret_cast<GetDebugInterfaceFn>(reinterpret_cast<void*>(::GetProcAddress(d3d12, "D3D12GetDebugInterface"))) : nullptr;
		if (!getDebug) {
			g_enableResult = "not enabled: d3d12.dll has no D3D12GetDebugInterface";
			Log::Warn("render: [Render] debug_layer {}", g_enableResult);
			return false;
		}
		ComPtr<ID3D12Debug> debug;
		HRESULT             hr = getDebug(__uuidof(ID3D12Debug), reinterpret_cast<void**>(debug.GetAddressOf()));
		if (SUCCEEDED(hr) && debug) {
			debug->EnableDebugLayer();
			g_layer = true;
			ComPtr<ID3D12Debug1> debug1;
			if (a_gpuValidation && SUCCEEDED(debug.As(&debug1))) {
				debug1->SetEnableGPUBasedValidation(TRUE);
				g_gbv = true;
			}
		} else {
			Log::Warn("render: D3D12 debug layer unavailable ({}): install Windows' optional feature \"Graphics Tools\" "
					  "(Settings > System > Optional features); DRED only",
				d3d::Hr(hr));
		}
		ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dred;
		if (SUCCEEDED(getDebug(__uuidof(ID3D12DeviceRemovedExtendedDataSettings), reinterpret_cast<void**>(dred.GetAddressOf()))) && dred) {
			dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			g_dred = true;
		}
		if (!g_layer && !g_dred) {
			g_enableResult = std::format("not enabled ({})", d3d::Hr(hr));
			return false;
		}
		g_enableResult = std::format("debug layer {}, GPU-based validation {}, DRED {}", g_layer ? "on" : "unavailable", g_gbv ? "on" : "off",
			g_dred ? "on" : "unavailable");
		Log::Warn("render: [Render] debug_layer: {} (slow: for validation runs only)", g_enableResult);
		g_enabled.store(true);
		return true;
	}

	bool Enabled() { return g_enabled.load(); }

	// Attach and Poll run on the present thread. They make their D3D12 calls without g_lock: a message callback runs on
	// whichever thread made the offending call, possibly under the runtime's own locks, and takes g_lock.
	void Attach(ID3D12Device* a_device)
	{
		if (!g_enabled.load() || !a_device || g_attached.exchange(true)) {
			return;
		}
		ComPtr<ID3D12Device>     device = a_device;
		ComPtr<ID3D12InfoQueue>  queue;
		ComPtr<ID3D12InfoQueue1> queue1;
		bool                     callback = false;
		std::string              result;
		if (FAILED(device.As(&queue))) {
			result = g_layer ? "Elden Ring's device has NO debug layer (it existed before debug_layer was enabled?)" : "no info queue (no debug layer)";
			Log::Warn("render: debug layer: {}", result);
		} else {
			// Never break into a debugger that isn't there.
			queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
			queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
			queue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, FALSE);
			D3D12_MESSAGE_SEVERITY  deny[] = { D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE };
			D3D12_INFO_QUEUE_FILTER filter{};
			filter.DenyList.NumSeverities = 2;
			filter.DenyList.pSeverityList = deny;
			queue->PushStorageFilter(&filter);
			queue->ClearStoredMessages();
			DWORD cookie = 0;
			callback = SUCCEEDED(queue.As(&queue1)) && SUCCEEDED(queue1->RegisterMessageCallback(&Callback, D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie));
			result = std::format("Elden Ring's device {} has the debug layer; messages {}", static_cast<void*>(a_device),
				callback ? "through the ID3D12InfoQueue1 callback" : "polled at Present (no ID3D12InfoQueue1)");
			Log::Info("render: debug layer: {}", result);
		}
		std::scoped_lock lock(g_lock);
		g_device = device;
		g_queue = queue;
		g_queue1 = queue1;
		g_callback = callback;
		g_attachResult = result;
	}

	void Poll()
	{
		if (!g_enabled.load()) {
			return;
		}
		ComPtr<ID3D12Device>    device;
		ComPtr<ID3D12InfoQueue> queue;
		{
			std::scoped_lock lock(g_lock);
			device = g_device;
			if (!g_callback) {
				queue = g_queue;
			}
		}
		if (!device) {
			return;
		}
		if (queue) {
			DrainStored(queue.Get());
		}
		if (!g_removedLogged.load(std::memory_order_relaxed)) {
			if (const HRESULT why = device->GetDeviceRemovedReason(); FAILED(why) && !g_removedLogged.exchange(true)) {
				Log::Error("d3d12: DEVICE REMOVED: {}", d3d::Hr(why));
				if (g_dred) {
					LogDred(device.Get());
				}
			}
		}
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		if (!g_enabled.load()) {
			return std::format("debug layer: {}", g_enableResult);
		}
		return std::format("debug layer: {} | {} | {}{}", g_enableResult, g_attachResult, g_filter.Summary(), g_removedLogged.load() ? " | DEVICE REMOVED (see the log)" : "");
	}
}
