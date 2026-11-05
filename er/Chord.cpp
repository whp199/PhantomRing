#include "Chord.h"

#include <algorithm>
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
		// View held this long opens the iDroid while still down; shorter (from viewHoldMs) at its release.
		const std::uint64_t     openMs = static_cast<std::uint64_t>(std::max(a_p.viewHoldMs, a_p.idroidOpenMs));

		// A View hold goes to MGSV: its Start for idroid_start_ms. A cut hold's close watch (cancelAt_, below) ends here -- the menu
		// this hold opens is the one the player asked for, not the cut one's (idroid_open_ms <= chord_window_ms only: a late Start
		// cut a hold MGSV was too busy to open, both let go, View held again within kCloseWatchMs).
		auto newHold = [&]() {
			guestStartUntil_ = now + static_cast<std::uint64_t>(a_p.idroidStartMs > 0 ? a_p.idroidStartMs : 0);
			cancelAt_ = 0;
			closeTaps_ = 0;
		};

		// Presses and releases.
		bool startPressed = false;
		auto edge = [&](Button& b, bool a_down) {
			if (a_down && !b.down) {
				startPressed = startPressed || &b == &start_;
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
				if (armed && b.tracked && !b.passed && !b.consumed && !b.toGuest && &b == &view_ && idroid &&
					now - b.downAt >= static_cast<std::uint64_t>(a_p.viewHoldMs)) {
					// View held past view_hold_ms and let go before idroid_open_ms (no item press, no chord): MGSV's iDroid now.
					b.toGuest = true;
					newHold();
					o.viewToGuest = true;
				} else if (armed && b.tracked && !b.passed && !b.consumed && !b.toGuest) {
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
		// A cut chord let go before it fired, MGSV's menu not open: its close watch ends (an MGSV menu after this is not the cut
		// hold's). Kept while the menu is open -- its close taps still go -- and after a fired chord (Tarnished mode: MGSV's menu,
		// still open, shows as kGuestMenu only once Snake is back).
		if (cancelAt_ != 0 && !start_.down && !view_.down && !fired_ && a_in.mode != Mode::kGuestMenu) {
			cancelAt_ = 0;
			closeTaps_ = 0;
		}

		// The chord: both down, neither passed on yet. Or (player report 2026-10-14: "the chord failed when View went down a bit
		// before Start") View's iDroid hold already went to MGSV and Start came within chord_window_ms of View: MGSV opens its
		// iDroid on that Start at once (live: 34 ms after the hold), and the player's Start would then be MGSV's, closing it
		// again or not. Now the chord wins: the hold's Start is cut, the real Start stays out of MGSV's pad, and MGSV's menu, if
		// it opened, gets a Start tap that closes it (below).
		const bool lateStart = startPressed && view_.down && view_.tracked && view_.toGuest && (a_in.mode == Mode::kSnake || a_in.mode == Mode::kGuestMenu) &&
		                       start_.downAt >= view_.downAt && start_.downAt - view_.downAt <= static_cast<std::uint64_t>(std::max(a_p.chordWindowMs, 0));
		if (chordOn && ((armed && start_.undecided() && view_.undecided()) || lateStart)) {
			if (view_.toGuest) {
				view_.toGuest = false;
				guestStartUntil_ = 0;
				cancelAt_ = now;
				closeTaps_ = 0;
				lastCloseAt_ = 0;
				o.idroidCancelled = true;
			}
			start_.tracked = view_.tracked = true;
			start_.consumed = view_.consumed = true;
			start_.chord = view_.chord = true;
			chordSince_ = now;
			fired_ = false;
		}
		// MGSV's menu, opened by the hold the chord cut: a Start tap closes it (MGSV: Start in its menu closes it; README), sent
		// kCloseGapMs after the cut so that MGSV sees the hold's Start released first, and once more if it is still open
		// kCloseRetryMs later. The chord still counts meanwhile.
		const bool closing = a_in.mode == Mode::kGuestMenu && cancelAt_ != 0 && now - cancelAt_ <= kCloseWatchMs;
		if (closing && closeTaps_ < 2 && now >= cancelAt_ + kCloseGapMs && (closeTaps_ == 0 || now >= lastCloseAt_ + kCloseRetryMs)) {
			++closeTaps_;
			lastCloseAt_ = now;
			guestCloseUntil_ = now + kTapMs;
			o.guestMenuClosed = true;
		}
		if ((armed || closing) && start_.down && view_.down && start_.chord && view_.chord && !fired_ &&
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
				if (now - b.downAt >= openMs) {
					b.toGuest = true;
					newHold();
					o.viewToGuest = true;
				}
			} else if (now - b.downAt >= wait) {
				b.passed = true;
				++o.menuPresses;
			}
		};
		hold(start_, false);
		hold(view_, idroid);

		// The quick item's shift ([Flask]): View down and still undecided -- or already the shift's -- when the item or cycle button
		// goes down. View is then the shift's (consumed: no map tap, no iDroid, no chord), and the press stays out of MGSV's pad
		// until released.
		const bool shiftAny = a_in.mode == Mode::kSnake && view_.down && view_.tracked && !view_.toGuest && !view_.chord && (view_.undecided() || view_.shift);
		const bool shiftOk = a_p.itemShift && shiftAny;
		// Tarnished mode: Elden Ring gets the item / cycle press itself (its X drinks); View, still undecided, then opens no map.
		const bool tarnishedShift = a_p.itemShift && a_in.mode == Mode::kTarnished && view_.down && view_.tracked && !view_.chord &&
		                            (view_.undecided() || view_.shift);
		auto shiftEdge = [&](bool a_down, bool& a_was, bool& a_held, int& a_count) {
			if (a_down && !a_was) {
				a_held = shiftOk;
				if (shiftOk) {
					++a_count;
					view_.consumed = true;
					view_.shift = true;
				} else if (tarnishedShift) {
					view_.consumed = true;
					view_.shift = true;
				}
			} else if (!a_down) {
				a_held = false;
			}
			a_was = a_down;
		};
		shiftEdge(a_in.item, itemDown_, itemHeld_, o.itemUses);
		shiftEdge(a_in.cycle, cycleDown_, cycleHeld_, o.itemCycles);
		o.guestMaskItem = itemHeld_;
		o.guestMaskCycle = cycleHeld_;
		// (0.7.0) View + the controls card button: the card again (the same shift; never in Tarnished mode, Elden Ring's D-pad there).
		if (a_in.help && !helpDown_) {
			helpHeld_ = a_p.helpShift && shiftAny;
			if (helpHeld_) {
				++o.helpPresses;
				view_.consumed = true;
				view_.shift = true;
			}
		} else if (!a_in.help) {
			helpHeld_ = false;
		}
		helpDown_ = a_in.help;
		o.guestMaskHelp = helpHeld_;

		// MGSV's menu opened on the iDroid hold's Start (at once, live 30 ms): the rest of that hold is dropped. Kept on, a menu
		// closed within guest_idroid_start_ms left kGuestMenu with the hold still running, and MGSV, back in kSnake, got its Start
		// again and reopened the iDroid (player 2026-11-04: B 1 s after it opened closed it, the Start 3 ms later reopened it 30 ms
		// after that). A hold whose menu never opens (MGSV busy) runs its full length as before; View stays the hold's (toGuest),
		// so a late Start still makes the chord and closes the menu (cancelAt_ above).
		if (a_in.mode == Mode::kGuestMenu && now < guestStartUntil_) {
			guestStartUntil_ = 0;
		}

		auto seen = [&](const Button& b) { return (b.down && (b.raw || b.passed)) || now < b.tapUntil; };
		if (a_in.mode != Mode::kGuestMenu) {
			o.erStart = seen(start_);
			o.erView = seen(view_);
		}
		o.guestStart = (a_in.mode == Mode::kSnake && now < guestStartUntil_) || (a_in.mode == Mode::kGuestMenu && now < guestCloseUntil_);
		// The real Start/View the chord holds stay out of MGSV's pad too, also in its own menu (a Start pressed in MGSV's menu
		// is MGSV's: it closes the iDroid).
		o.guestMaskStart = start_.down && start_.chord;
		o.guestMaskView = view_.down && view_.chord;
		return o;
	}
}
