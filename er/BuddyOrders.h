#pragma once

// D-Walker's orders, made visible in Elden Ring (player report: "it does not react to my commands and it's not clear they
// are given or not"). MGSV's own call menu gives the orders: hold LB, point the right stick at an entry, let the stick go
// (the entry stays selected), CLICK the right stick in while LB is still held (releasing LB cancels; a click with the
// stick still pushed is ignored: VERIFIED live; docs/GUEST.md 18.3). The player's sessions had no order at all. So:
//  - an order MGSV took (kGEvBuddyOrder, protocol v7) is shown in the corner ("D-Walker: Search Mode -- marks enemies");
//  - while LB is held a moment with the walker out, the corner says how to give one (until two orders were given);
//  - a hold that pointed at the walker's entries (the right half of the menu) and ended with no order says so.
// The pad is the real one (InputBridge sees it before any mask); nothing here changes what MGSV or Elden Ring get.

#include "phantomring_protocol.h"

#include <cstdint>
#include <string>

namespace phantom::er
{
	struct Frame;

	namespace BuddyOrders
	{
		void        OnOrder(const proto::GuestEvent& a_event);  // kGEvBuddyOrder (FrameBegin, er/Events.cpp)
		void        Tick(const Frame& a_frame);                 // FrameBegin, after the events
		std::string Describe();
		// The corner text for an order (pure: tests).
		std::string OrderText(std::uint32_t a_command, std::uint32_t a_buddyType);

		// Pure (tests/test_host.cpp): the hint state of one LB hold.
		class Hint
		{
		public:
			enum class Say
			{
				kNothing,
				kHowTo,    // LB held a moment: how to give an order
				kNoOrder,  // LB let go after pointing at the walker's entries, and no order came
			};
			static constexpr std::uint64_t kHoldMs = 600;      // a deliberate hold (the menu is up), not a tap
			static constexpr std::uint64_t kAnswerMs = 800;    // an order arrives a few frames after the click
			static constexpr std::uint32_t kLearnedAfter = 2;  // orders given: the how-to is no longer shown
			// a_rx: the right stick's x, -1..1 (the walker's entries are on the menu's right). a_orders: orders seen so far.
			Say Update(std::uint64_t a_nowMs, bool a_eligible, bool a_lb, float a_rx, std::uint32_t a_orders);

		private:
			bool          down_{ false };
			bool          howToShown_{ false };
			bool          pointed_{ false };
			bool          waiting_{ false };
			std::uint64_t downAt_{ 0 };
			std::uint64_t upAt_{ 0 };
			std::uint32_t ordersAtDown_{ 0 };
		};
	}
}
