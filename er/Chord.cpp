#include "Chord.h"

#include <initializer_list>

namespace phantom::er
{
	PadGate::Output PadGate::Update(const Input& a_in, const Params& a_p)
	{
		Output                  o;
		const std::uint64_t     now = a_in.nowMs;
		const bool              armed = a_in.mode == Mode::kSnake || a_in.mode == Mode::kTarnished;
		const bool              chordOn = a_p.chordMs > 0;
		const std::uint64_t     wait = chordOn && a_p.startWaitMs > 0 ? static_cast<std::uint64_t>(a_p.startWaitMs) : 0;
		const bool              idroid = a_in.mode == Mode::kSnake && a_p.viewHoldMs > 0;  // else View works like Start

		// Presses and releases.
		auto edge = [&](Button& b, bool a_down) {
			if (a_down && !b.down) {
				const auto tap = b.tapUntil;
				b = Button{};
				b.tapUntil = tap;
				b.down = true;
				b.downAt = now;
				b.tracked = armed;
				b.raw = a_in.mode == Mode::kOff;
				// MGSV's menu: Elden Ring never sees this press, not even when the menu closes before it is released (it would
				// open Elden Ring's menu or map then).
				b.consumed = a_in.mode == Mode::kGuestMenu;
				if (b.raw) {
					++o.menuPresses;
				}
			} else if (!a_down && b.down) {
				if (armed && b.tracked && !b.passed && !b.consumed && !b.toGuest) {
					b.tapUntil = now + kTapMs;  // released before the wait: Elden Ring gets the tap now
					++o.menuPresses;
				}
				b.down = false;
				b.chord = false;
			}
		};
		edge(start_, a_in.start);
		edge(view_, a_in.view);

		if (!armed) {
			// A menu came up (or Snake stopped) while a press was still undecided: it reaches no one until released.
			for (Button* b : { &start_, &view_ }) {
				if (b->undecided()) {
					b->consumed = true;
				}
			}
		}

		// The chord: both down, neither passed on yet (nor View's iDroid hold done).
		if (armed && chordOn && start_.undecided() && view_.undecided()) {
			start_.consumed = view_.consumed = true;
			start_.chord = view_.chord = true;
			chordSince_ = now;
			fired_ = false;
		}
		if (armed && start_.down && view_.down && start_.chord && view_.chord && !fired_ &&
			now - chordSince_ >= static_cast<std::uint64_t>(a_p.chordMs)) {
			fired_ = true;
			o.chord = true;
		}

		// Held on: Start (and View in kTarnished) reaches Elden Ring after the wait; View in kSnake becomes MGSV's iDroid.
		auto hold = [&](Button& b, bool a_idroid) {
			if (!armed || !b.undecided()) {
				return;
			}
			if (a_idroid) {
				if (now - b.downAt >= static_cast<std::uint64_t>(a_p.viewHoldMs)) {
					b.toGuest = true;
					guestStartUntil_ = now + static_cast<std::uint64_t>(a_p.idroidStartMs > 0 ? a_p.idroidStartMs : 0);
					o.viewToGuest = true;
				}
			} else if (now - b.downAt >= wait) {
				b.passed = true;
				++o.menuPresses;
			}
		};
		hold(start_, false);
		hold(view_, idroid);

		auto seen = [&](const Button& b) { return (b.down && (b.raw || b.passed)) || now < b.tapUntil; };
		if (a_in.mode != Mode::kGuestMenu) {
			o.erStart = seen(start_);
			o.erView = seen(view_);
		}
		o.guestStart = a_in.mode == Mode::kSnake && now < guestStartUntil_;
		return o;
	}
}
