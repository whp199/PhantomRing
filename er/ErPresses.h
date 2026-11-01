#pragma once

// Elden Ring-only button presses (InputBridge::PressErButtons: the hidden Tarnished uses or cycles his quick item, er/QuickItem.cpp).
// Each button keeps its own deadline, so a second press never cuts the first one short: before the 0.6.0 review one slot held
// the buttons and a later press replaced it -- the quick item's cycle retry (400 ms after View + D-pad down) landing on a View + X
// cut the hidden Tarnished's X to under a frame. Lock-free: the game thread presses, the XInput hook reads.
// Pure: tests/test_host_quickitem.inc.

#include <array>
#include <atomic>
#include <cstdint>

namespace phantom::er
{
	class ErPresses
	{
	public:
		// a_buttons held until a_untilMs (a button already held longer keeps its own deadline).
		void Press(std::uint16_t a_buttons, std::uint64_t a_untilMs)
		{
			for (unsigned i = 0; i < 16; ++i) {
				if (a_buttons & (1u << i)) {
					auto& u = until_[i];
					auto  cur = u.load(std::memory_order_relaxed);
					while (cur < a_untilMs && !u.compare_exchange_weak(cur, a_untilMs, std::memory_order_release, std::memory_order_relaxed)) {
					}
				}
			}
		}

		// The buttons still held at a_nowMs.
		[[nodiscard]] std::uint16_t Held(std::uint64_t a_nowMs) const
		{
			std::uint16_t out = 0;
			for (unsigned i = 0; i < 16; ++i) {
				if (a_nowMs < until_[i].load(std::memory_order_acquire)) {
					out = static_cast<std::uint16_t>(out | (1u << i));
				}
			}
			return out;
		}

	private:
		std::array<std::atomic<std::uint64_t>, 16> until_{};
	};
}
