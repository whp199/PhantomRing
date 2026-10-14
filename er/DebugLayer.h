#pragma once

// [Render] debug_layer (docs/ROADMAP.md W1/W5; off by default, UNVERIFIED live): Microsoft's D3D12 debug layer,
// GPU-based validation and DRED (device-removed breadcrumbs + page faults) for a tester on native Windows D3D12, where
// a resource-state mistake that vkd3d-proton forgives removes the device. Validation messages go to the host log
// through ID3D12InfoQueue (a callback through ID3D12InfoQueue1 where the runtime has it, else polled at Present).
//
// The layer must be on before the first D3D12 device of the process exists (enabling it later removes that device),
// so Enable runs first thing in the plugin's init thread, and refuses (logged) once the process has a window: Elden
// Ring creates its window before its device. Needs Windows' "Graphics Tools" optional feature (d3d12sdklayers.dll);
// without it D3D12GetDebugInterface fails and only DRED is on.

#include "D3D12Util.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace phantom::er::DebugLayer
{
	// Before any D3D12 device: debug layer (+ GPU-based validation) and DRED. False (logged) if nothing could be enabled.
	bool Enable(bool a_gpuValidation);
	[[nodiscard]] bool Enabled();

	// Elden Ring's device (the renderer's, once the presenting queue is known): its info queue -> the host log.
	void Attach(ID3D12Device* a_device);
	// Each Present: stored messages (when there is no callback), and once the device is removed, its reason plus DRED.
	void Poll();
	std::string Describe();

	// Messages logged while this is alive on a thread are tagged "during PhantomRing's work" (the renderer's Present
	// callback; Elden Ring's own calls, and GPU-based validation's reports, carry no tag).
	struct OurScope
	{
		OurScope();
		~OurScope();
		OurScope(const OurScope&) = delete;
		OurScope& operator=(const OurScope&) = delete;
	};

	// ---- pure (tests/test_render.cpp) ----

	[[nodiscard]] const char* SeverityName(int a_severity);  // D3D12_MESSAGE_SEVERITY

	// Which messages reach the log: corruption, errors and warnings; at most a_perId lines per message id and a_total
	// lines in all (a validation error repeats every frame), every message counted.
	class MessageFilter
	{
	public:
		explicit MessageFilter(int a_perId = 3, int a_total = 400) :
			m_perId(a_perId), m_total(a_total) {}

		// True: write this one to the log. a_suppressedFirst: set when this is the first message of its id not logged.
		bool Take(int a_severity, int a_id, bool* a_suppressedFirst = nullptr);
		[[nodiscard]] std::uint64_t Count(int a_severity) const;
		[[nodiscard]] std::uint64_t Logged() const { return m_logged; }
		[[nodiscard]] std::uint64_t Dropped() const { return m_dropped; }
		[[nodiscard]] std::string   Summary() const;  // "2 corruption, 5 errors, 40 warnings (12 ids; 30 logged)"

	private:
		int                               m_perId;
		int                               m_total;
		std::uint64_t                     m_counts[5]{};
		std::uint64_t                     m_logged{ 0 };
		std::uint64_t                     m_dropped{ 0 };
		std::unordered_map<int, unsigned> m_perIdSeen;
	};
}
