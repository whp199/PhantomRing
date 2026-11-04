// Included by tests/test_host.cpp: Elden Ring's quick item without leaving Snake (roadmap 2.10, [Flask]): the View shift of the
// pad gate (er/Chord.h) and what the quick slot allows (er/QuickItemLogic.h).
#include "Chord.h"
#include "QuickItemLogic.h"

namespace
{
	// The gate polled every 5 ms with View, the item button (X) and the cycle button (D-pad down).
	struct ShiftRun
	{
		PadGate         gate;
		PadGate::Params params;
		std::uint64_t   t{ 1000 };
		int             uses{ 0 }, cycles{ 0 }, toGuest{ 0 }, chords{ 0 };
		bool            sawErView{ false }, maskItem{ false }, maskCycle{ false };
		std::uint64_t   itemToGuestMs{ 0 };  // how long MGSV's pad carried the item button (unmasked) while it was down
		std::uint64_t   guestStartMs{ 0 };   // how long MGSV's pad carried the iDroid's synthesized Start
		std::uint64_t   firstToGuest{ 0 };   // when the iDroid went to MGSV

		ShiftRun() { params.itemShift = true; }  // the defaults otherwise: view_hold_ms 400, idroid_open_ms 1000

		void Step(bool a_view, bool a_item, bool a_cycle, std::uint64_t a_ms, PadGate::Mode a_mode = PadGate::Mode::kSnake, bool a_start = false)
		{
			for (std::uint64_t e = 0; e < a_ms; e += 5) {
				PadGate::Input in;
				in.nowMs = t;
				in.start = a_start;
				in.view = a_view;
				in.item = a_item;
				in.cycle = a_cycle;
				in.mode = a_mode;
				const auto o = gate.Update(in, params);
				uses += o.itemUses;
				cycles += o.itemCycles;
				toGuest += o.viewToGuest ? 1 : 0;
				if (o.viewToGuest && !firstToGuest) {
					firstToGuest = t;
				}
				guestStartMs += o.guestStart ? 5 : 0;
				chords += o.chord ? 1 : 0;
				sawErView |= o.erView;
				maskItem = o.guestMaskItem;
				maskCycle = o.guestMaskCycle;
				itemToGuestMs += a_item && !o.guestMaskItem ? 5 : 0;
				t += 5;
			}
		}
	};
}

static void TestQuickItem()
{
	using M = PadGate::Mode;
	{  // (0.7.0) spirit ashes: summoned when Elden Ring's HUD counts more spirits or an ally appeared; refused after kSpiritWaitMs
		using phantom::er::quickitem::SpiritCheck;
		using SO = phantom::er::quickitem::SpiritOutcome;
		CHECK(SpiritCheck(0, 0, 0, 1000) == SO::kWaiting);
		CHECK(SpiritCheck(0, 1, 0, 2600) == SO::kSummoned);
		CHECK(SpiritCheck(-1, -1, 2, 2600) == SO::kSummoned);   // the HUD count unreadable: the allies tell
		CHECK(SpiritCheck(1, 1, 0, 4499) == SO::kWaiting);
		CHECK(SpiritCheck(1, 1, 0, 4500) == SO::kRefused);
		CHECK(SpiritCheck(-1, 3, 0, 5000) == SO::kRefused);     // no count before: a later count proves nothing
	}
	{  // (0.7.0) View + D-pad up: the controls card again; kept from MGSV; View opens nothing; not in Tarnished mode
		PadGate         gate;
		PadGate::Params p;
		p.helpShift = true;
		std::uint64_t t = 1000;
		int           help = 0;
		bool          mask = false, erView = false;
		auto step = [&](bool a_view, bool a_help, std::uint64_t a_ms, M a_mode) {
			for (std::uint64_t e = 0; e < a_ms; e += 5) {
				PadGate::Input in;
				in.nowMs = t;
				in.view = a_view;
				in.help = a_help;
				in.mode = a_mode;
				const auto o = gate.Update(in, p);
				help += o.helpPresses;
				mask = o.guestMaskHelp;
				erView |= o.erView;
				t += 5;
			}
		};
		step(true, false, 80, M::kSnake);
		step(true, true, 100, M::kSnake);
		CHECK(help == 1 && mask);
		step(false, false, 300, M::kSnake);
		CHECK(help == 1 && !mask && !erView);  // no map with it
		step(false, true, 100, M::kSnake);      // D-pad up alone: MGSV's primary weapon, no card
		step(false, false, 100, M::kSnake);
		CHECK(help == 1);
		step(true, false, 80, M::kTarnished);
		step(true, true, 100, M::kTarnished);
		CHECK(help == 1 && !mask);
		step(false, false, 300, M::kTarnished);
		p.helpShift = false;                    // [Overlay] controls_card = off / card_button = none
		step(true, false, 80, M::kSnake);
		step(true, true, 100, M::kSnake);
		CHECK(help == 1 && !mask);
	}
	{  // View, then X 100 ms later: one use; X stays out of MGSV's pad until released; View opens no map and no iDroid
		ShiftRun r;
		r.Step(true, false, false, 100);
		r.Step(true, true, false, 150);
		CHECK(r.uses == 1 && r.maskItem && r.itemToGuestMs == 0);
		r.Step(true, false, false, 600);  // View held on past the iDroid's 400 ms: still the shift's
		CHECK(!r.maskItem && r.toGuest == 0);
		r.Step(false, false, false, 300);
		CHECK(r.uses == 1 && r.toGuest == 0 && !r.sawErView);
	}
	{  // View held, X twice: two uses (a second drink); D-pad down: one cycle, kept from MGSV
		ShiftRun r;
		r.Step(true, false, false, 50);
		r.Step(true, true, false, 100);
		r.Step(true, false, false, 100);
		r.Step(true, true, false, 100);
		r.Step(true, false, true, 100);
		CHECK(r.uses == 2 && r.cycles == 1 && r.maskCycle);
		r.Step(false, false, false, 100);
		CHECK(!r.maskCycle && r.toGuest == 0 && !r.sawErView);
	}
	{  // View and X in the same poll (a keyboard key mapped to both): a use
		ShiftRun r;
		r.Step(true, true, false, 50);
		CHECK(r.uses == 1 && r.maskItem);
	}
	{  // X first (MGSV's dive), View after: no use; View tapped still opens the map at its release
		ShiftRun r;
		r.Step(false, true, false, 50);
		r.Step(true, true, false, 100);
		CHECK(r.uses == 0 && !r.maskItem && r.itemToGuestMs == 150);
		r.Step(false, false, false, 200);
		CHECK(r.sawErView && r.uses == 0);
	}
	{  // View held 600 ms, then X (0.6.0 edge case: "hold View, then press X" past the iDroid's 400 ms): a drink, X kept from MGSV,
	   // no iDroid -- not then, not at View's release -- and no map
		ShiftRun r;
		r.Step(true, false, false, 600);
		CHECK(r.toGuest == 0 && r.guestStartMs == 0);
		r.Step(true, true, false, 150);
		CHECK(r.uses == 1 && r.maskItem && r.itemToGuestMs == 0);
		r.Step(true, false, false, 1500);  // View held on long after: still the shift's
		r.Step(false, false, false, 300);
		CHECK(r.uses == 1 && r.toGuest == 0 && r.guestStartMs == 0 && !r.sawErView);
	}
	{  // ...X at 950 ms (just before idroid_open_ms): still a drink; D-pad down at 700 ms: the next item
		ShiftRun r;
		r.Step(true, false, false, 950);
		r.Step(true, true, false, 100);
		CHECK(r.uses == 1 && r.toGuest == 0);
		ShiftRun c;
		c.Step(true, false, false, 700);
		c.Step(true, false, true, 100);
		CHECK(c.cycles == 1 && c.maskCycle && c.toGuest == 0);
		c.Step(false, false, false, 300);
		CHECK(c.toGuest == 0 && !c.sawErView);
	}
	{  // View held 1 s: the iDroid opens while it is still held (MGSV gets Start held 1.2 s); an X after that is MGSV's (its menu)
		ShiftRun r;
		const auto down = r.t;
		r.Step(true, false, false, 1100);
		CHECK(r.toGuest == 1 && r.firstToGuest >= down + 1000 && r.firstToGuest <= down + 1010);
		r.Step(true, true, false, 100);
		CHECK(r.uses == 0 && !r.maskItem);
	}
	{  // View held 600 ms and let go (no X): the iDroid at the release, Start held 1.2 s for MGSV; Elden Ring no map
		ShiftRun r;
		r.Step(true, false, false, 600);
		CHECK(r.toGuest == 0);
		const auto up = r.t;
		r.Step(false, false, false, 1500);
		CHECK(r.toGuest == 1 && r.firstToGuest == up && r.guestStartMs >= 1195 && r.guestStartMs <= 1205 && !r.sawErView && r.uses == 0);
	}
	{  // View let go at 395 ms: still a tap (Elden Ring's map), no iDroid
		ShiftRun r;
		r.Step(true, false, false, 395);
		r.Step(false, false, false, 300);
		CHECK(r.sawErView && r.toGuest == 0 && r.guestStartMs == 0);
	}
	{  // View, then Start 900 ms later (the iDroid not open yet): the Tarnished-mode chord, no iDroid
		ShiftRun r;
		r.Step(true, false, false, 900);
		r.Step(true, false, false, 1100, M::kSnake, true);
		CHECK(r.chords == 1 && r.toGuest == 0 && r.guestStartMs == 0 && r.uses == 0);
		r.Step(false, false, false, 300);
		CHECK(r.toGuest == 0 && !r.sawErView);
	}
	{  // idroid_open_ms 0: the iDroid at view_hold_ms as before 0.6.0 -- then X is MGSV's (its menu), no use
		ShiftRun r;
		r.params.idroidOpenMs = 0;
		r.Step(true, false, false, 500);
		CHECK(r.toGuest == 1);
		r.Step(true, true, false, 100);
		CHECK(r.uses == 0 && !r.maskItem);
	}
	{  // A View tap alone: Elden Ring's map as before
		ShiftRun r;
		r.Step(true, false, false, 100);
		r.Step(false, false, false, 200);
		CHECK(r.sawErView && r.uses == 0 && r.toGuest == 0);
	}
	{  // Not in Tarnished mode (Elden Ring has X itself: its own drink), nor with the shift off ([Flask] enabled = false). In
	   // Tarnished mode View, still undecided when X went down, then opens no map at its release (nor while held).
		ShiftRun r;
		r.Step(true, false, false, 50, M::kTarnished);
		r.Step(true, true, false, 100, M::kTarnished);
		CHECK(r.uses == 0 && !r.maskItem);
		r.Step(true, false, false, 600, M::kTarnished);
		r.Step(false, false, false, 300, M::kTarnished);
		CHECK(!r.sawErView && r.uses == 0 && r.toGuest == 0);
		ShiftRun d;  // ...D-pad down (its own next item) the same
		d.Step(true, false, false, 100, M::kTarnished);
		d.Step(true, false, true, 100, M::kTarnished);
		d.Step(false, false, false, 300, M::kTarnished);
		CHECK(!d.sawErView && d.cycles == 0 && !d.maskCycle);
		ShiftRun m;  // ...a View tap alone there is still its map
		m.Step(true, false, false, 100, M::kTarnished);
		m.Step(false, false, false, 300, M::kTarnished);
		CHECK(m.sawErView);
		ShiftRun off;
		off.params.itemShift = false;
		off.Step(true, false, false, 50);
		off.Step(true, true, false, 100);
		CHECK(off.uses == 0 && !off.maskItem);
	}
	{  // View + Start still makes the Tarnished-mode chord; X after the chord formed is no use
		ShiftRun r;
		r.Step(true, false, false, 50, M::kSnake, false);
		r.Step(true, false, false, 1100, M::kSnake, true);
		CHECK(r.chords == 1);
		r.Step(true, true, false, 100, M::kSnake, true);
		CHECK(r.uses == 0);
	}
	{  // After a use, a Start while View is still held is a plain Start (no chord: View is the shift's)
		ShiftRun r;
		r.Step(true, true, false, 50);
		r.Step(true, false, false, 50);
		r.Step(true, false, false, 1200, M::kSnake, true);
		CHECK(r.uses == 1 && r.chords == 0);
	}

	// What the quick slot allows.
	using namespace quickitem;
	{
		Goods flask{ 1001, true, 10, 0, true };
		CHECK(Classify(flask, 4) == Verdict::kUse);
		CHECK(Classify(flask, 0) == Verdict::kEmpty);  // none left
		Goods empty{ 1000, true, kAnimEmpty, 0, false };
		CHECK(Classify(empty, 3) == Verdict::kEmpty);  // the empty twin
		CHECK(Classify(Goods{ 130, true, 32, 9, true }, 1) == Verdict::kScreen);   // Spectral Steed Whistle
		CHECK(Classify(Goods{ 115, true, 6, 1, false }, 1) == Verdict::kScreen);   // Memory of Grace
		CHECK(Classify(Goods{ 2040, true, kAnimTelescope, 0, false }, 1) == Verdict::kScreen);  // Telescope
		CHECK(Classify(Goods{ 200000, true, 34, kMenuSpiritAshes, false }, 1) == Verdict::kUse);  // spirit ashes (the bell)
		CHECK(Classify(Goods{ 2140, true, 64, 0, false }, 1) == Verdict::kUse);   // Margit's Shackle
		CHECK(Classify(Goods{}, 0) == Verdict::kNoItem);
		// No EquipParamGoods row (the table not resolved: another regulation or build): only flasks are pressed for Snake; Memory of
		// Grace, the whistle, the telescope and anything else are Tarnished mode only (review of 0.6.0: they were pressed under the
		// hidden Tarnished, the whistle calling Torrent under the puppet).
		CHECK(Classify(Goods{ 1001, false }, 0) == Verdict::kEmpty && Classify(Goods{ 1001, false }, 3) == Verdict::kUse);
		CHECK(Classify(Goods{ 250, false }, 1) == Verdict::kUse && Classify(Goods{ 1051, false }, 2) == Verdict::kUse);
		for (const int id : { 115, 130, 181, 2040, 2050, 2070, 300, 200000 }) {
			CHECK(Classify(Goods{ id, false }, 1) == Verdict::kScreen);
		}
		CHECK(Name(1001) == "Flask of Crimson Tears" && Name(1007) == "Flask of Crimson Tears +3" && Name(1051) == "Flask of Cerulean Tears");
		CHECK(FlaskTwins(1000, 1001) && FlaskTwins(1007, 1006) && FlaskTwins(250, 251) && FlaskTwins(1050, 1051) && !FlaskTwins(1001, 1003) &&
		      !FlaskTwins(1001, 1051) && !FlaskTwins(300, 301) && FlaskTwins(300, 300));
		CHECK(Shown(201004) == "Spirit ashes" && Shown(99) == "Quick item" && IsFlask(250) && !IsFlask(300));
		// The switch's corner line (live: "(1)" for the drunk-out Cerulean flask, its empty twin 1050 counting 1).
		CHECK(CycleText(1051, 1, false) == "Quick item: Flask of Cerulean Tears (1)");
		CHECK(CycleText(1050, 1, true) == "Quick item: Flask of Cerulean Tears (empty -- rest at a grace)");
		CHECK(CycleText(300, 0, true).ends_with("(none left)"));
		CHECK(CycleText(-1, 0, false) == "Quick item: (empty slot)");
	}
	{  // A flask's count drops: used, 3 left
		UseTracker t;
		t.Begin(Slot{ 1001, 4, false }, true, 0);
		CHECK(t.Update(Slot{ 1001, 4, false }, 500, 3000) == UseTracker::Outcome::kPending);
		CHECK(t.Update(Slot{ 1001, 3, false }, 1200, 3000) == UseTracker::Outcome::kUsed && t.Left() == 3);
		CHECK(t.Update(Slot{ 1001, 3, false }, 1300, 3000) == UseTracker::Outcome::kIdle);
	}
	{  // The last one: the slot moves on to the empty twin
		UseTracker t;
		t.Begin(Slot{ 1001, 1, false }, true, 0);
		CHECK(t.Update(Slot{ 1000, 1, true }, 900, 3000) == UseTracker::Outcome::kUsed && t.Left() == 0);
	}
	{  // Elden Ring didn't use it (the count never dropped)
		UseTracker t;
		t.Begin(Slot{ 1001, 4, false }, true, 0);
		CHECK(t.Update(Slot{ 1001, 4, false }, 2999, 3000) == UseTracker::Outcome::kPending);
		CHECK(t.Update(Slot{ 1001, 4, false }, 3000, 3000) == UseTracker::Outcome::kNotUsed);
	}
	{  // Elden Ring-only presses keep a deadline per button (review of 0.6.0): the cycle retry 400 ms after a View + D-pad down
	   // no longer cuts a View + X's press short.
		ErPresses p;
		CHECK(p.Held(0) == 0);
		p.Press(0x0004 /* X: the item */, 1000 + 150);
		p.Press(0x0002 /* D-pad down: the cycle retry */, 1050 + 150);
		CHECK(p.Held(1060) == 0x0006);
		CHECK(p.Held(1160) == 0x0002);  // X's own 150 ms are over, D-pad down's not
		CHECK(p.Held(1200) == 0);
		p.Press(0x0004, 2000);
		p.Press(0x0004, 1500);  // a shorter press of a held button doesn't shorten it
		CHECK(p.Held(1900) == 0x0004);
	}
	{  // Not a consumable: taken as used at once
		UseTracker t;
		t.Begin(Slot{ 2140, 1, false }, false, 0);
		CHECK(t.Update(Slot{ 2140, 1, false }, 16, 3000) == UseTracker::Outcome::kAssumed);
	}
}
