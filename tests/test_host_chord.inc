// Included by tests/test_host.cpp: Start/View and the Tarnished-mode chord (er/Chord.h), the fall watchdog (er/Kinematic.h
// FallWatch) against the synthetic world.
#include "Chord.h"
#include "Ladder.h"
#include "VirtualPad.h"

namespace
{
	// Polls the gate every 5 ms, as Elden Ring's input thread does (faster than its frames).
	struct GateRun
	{
		PadGate         gate;
		PadGate::Params params;
		std::uint64_t   t{ 1000 };
		int             presses{ 0 }, chords{ 0 }, toGuest{ 0 }, cancels{ 0 }, closes{ 0 };
		bool            erStart{ false }, erView{ false }, guestStart{ false }, maskStart{ false };
		std::uint64_t   guestStartMs{ 0 };  // how long MGSV's pad carried the synthesized Start since Clear()
		std::uint64_t   guestRealStartMs{ 0 };  // ...and the real Start, unmasked, in MGSV's menu (MGSV takes it there)
		bool            sawErStart{ false }, sawErView{ false }, sawGuestStart{ false };
		std::uint64_t   firstErStart{ 0 };  // when Elden Ring first saw Start since Clear()

		// These cases were written for the iDroid opening at view_hold_ms while View is still held (idroid_open_ms 0, before
		// 0.6.0): the iDroid-then-chord timings below need it there. TestQuickItem has the default (at the release, or 1 s).
		GateRun() { params.idroidOpenMs = 0; }

		void Step(bool a_start, bool a_view, PadGate::Mode a_mode, std::uint64_t a_ms)
		{
			for (std::uint64_t e = 0; e < a_ms; e += 5) {
				PadGate::Input in;
				in.nowMs = t;
				in.start = a_start;
				in.view = a_view;
				in.mode = a_mode;
				const auto o = gate.Update(in, params);
				presses += o.menuPresses;
				chords += o.chord ? 1 : 0;
				toGuest += o.viewToGuest ? 1 : 0;
				cancels += o.idroidCancelled ? 1 : 0;
				closes += o.guestMenuClosed ? 1 : 0;
				maskStart = o.guestMaskStart;
				guestStartMs += o.guestStart ? 5 : 0;
				guestRealStartMs += a_start && a_mode == PadGate::Mode::kGuestMenu && !o.guestMaskStart ? 5 : 0;
				erStart = o.erStart;
				erView = o.erView;
				guestStart = o.guestStart;
				if (o.erStart && !sawErStart) {
					firstErStart = t;
				}
				sawErStart |= o.erStart;
				sawErView |= o.erView;
				sawGuestStart |= o.guestStart;
				t += 5;
			}
		}
		void Clear()
		{
			presses = chords = toGuest = cancels = closes = 0;
			guestStartMs = guestRealStartMs = 0;
			sawErStart = sawErView = sawGuestStart = false;
			firstErStart = 0;
		}
	};
}

static void TestChord()
{
	using M = PadGate::Mode;
	{  // Snake plays: a Start tap still opens Elden Ring's menu -- at the release (before the 150 ms wait ran out)
		GateRun r;
		r.Step(true, false, M::kSnake, 80);
		CHECK(!r.sawErStart && r.presses == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(r.sawErStart && r.presses == 1 && r.chords == 0 && !r.erStart);  // a 120 ms press, then released
	}
	{  // Start held: Elden Ring gets it 400 ms after it went down (room for the chord), for as long as it is held
		GateRun r;
		const auto down = r.t;
		r.Step(true, false, M::kSnake, 600);
		CHECK(r.erStart && r.presses == 1 && r.firstErStart >= down + 400 && r.firstErStart <= down + 410);
		r.Step(false, false, M::kSnake, 200);
		CHECK(!r.erStart && r.presses == 1);
	}
	{  // View tapped: Elden Ring's map at the release; held 400 ms: MGSV's iDroid (Start held 1.2 s), Elden Ring nothing
		GateRun r;
		r.Step(false, true, M::kSnake, 100);
		CHECK(!r.sawErView);
		r.Step(false, false, M::kSnake, 200);
		CHECK(r.sawErView && r.presses == 1 && r.toGuest == 0 && !r.sawGuestStart);
		r.Clear();
		r.Step(false, true, M::kSnake, 700);
		CHECK(r.toGuest == 1 && r.guestStart && !r.sawErView && r.presses == 0);
		r.Step(false, false, M::kSnake, 1500);
		CHECK(!r.guestStart && !r.sawErView && r.presses == 0);
	}
	{  // the chord, pressed together: fires once after 1 s, neither reaches Elden Ring nor MGSV, held on it doesn't repeat
		GateRun r;
		r.Step(true, true, M::kSnake, 990);
		CHECK(r.chords == 0 && r.gate.Holding());
		r.Step(true, true, M::kSnake, 3000);
		CHECK(r.chords == 1 && !r.sawErStart && !r.sawErView && !r.sawGuestStart && r.presses == 0 && r.toGuest == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && !r.sawErView && r.presses == 0 && !r.gate.Holding());
	}
	{  // View first (iDroid hold under way), Start 200 ms later: the chord, the iDroid hold cancelled
		GateRun r;
		r.Step(false, true, M::kSnake, 200);
		r.Step(true, true, M::kSnake, 1100);
		CHECK(r.chords == 1 && r.toGuest == 0 && !r.sawGuestStart && !r.sawErStart && !r.sawErView);
		r.Step(false, false, M::kSnake, 300);
		CHECK(r.presses == 0 && !r.sawErView);
	}
	{  // Start first, View within the wait: the chord; released one by one: no tap afterwards
		GateRun r;
		r.Step(true, false, M::kSnake, 100);
		r.Step(true, true, M::kSnake, 1100);
		CHECK(r.chords == 1);
		r.Step(true, false, M::kSnake, 400);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && !r.sawErView && r.presses == 0);
	}
	{  // Start first, View 300 ms later (inside the 400 ms wait): the chord
		GateRun r;
		r.Step(true, false, M::kSnake, 300);
		r.Step(true, true, M::kSnake, 1100);
		CHECK(r.chords == 1 && !r.sawErStart && !r.sawErView && r.presses == 0 && r.toGuest == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && !r.sawErView && r.presses == 0);
	}
	{  // the player's case: View a bit before Start -- the iDroid hold already went to MGSV (400 ms), Start 550 ms after View:
	   // still the chord; MGSV's synthesized Start is cut after ~150 ms (it opens the iDroid only after ~1 s of it)
		GateRun r;
		r.Step(false, true, M::kSnake, 550);
		CHECK(r.toGuest == 1 && r.guestStart);
		r.Step(true, true, M::kSnake, 1100);
		CHECK(r.chords == 1 && r.cancels == 1 && !r.guestStart && r.guestStartMs <= 160 && !r.sawErStart && !r.sawErView && r.presses == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(r.presses == 0 && !r.sawErView && !r.gate.Holding());
	}
	{  // ...but Start 900 ms after View (past chord_window_ms 700): View's iDroid stays, no chord
		GateRun r;
		r.Step(false, true, M::kSnake, 900);
		r.Step(true, true, M::kSnake, 1500);
		CHECK(r.chords == 0 && r.cancels == 0 && r.toGuest == 1 && r.guestStartMs >= 1100);
		r.Step(false, false, M::kSnake, 300);
	}
	{  // the cut hold opened MGSV's menu anyway: it gets one Start tap to close it (the held Start stays out of its pad), and
	   // the chord still fires
		GateRun r;
		r.Step(false, true, M::kSnake, 500);
		r.Step(true, true, M::kSnake, 200);
		CHECK(r.cancels == 1 && r.maskStart);
		r.Clear();
		r.Step(true, true, M::kGuestMenu, 400);
		CHECK(r.closes == 1 && r.guestStartMs >= 115 && r.guestStartMs <= 125 && r.maskStart && !r.guestStart);
		r.Step(true, true, M::kSnake, 600);
		CHECK(r.chords == 1 && r.closes == 1 && !r.sawErStart && !r.sawErView);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.maskStart && r.presses == 0);
	}
	{  // as live: the hold opens MGSV's iDroid at once (34 ms after it), Start comes 550 ms after View in MGSV's menu -- the chord:
	   // the real Start stays out of MGSV's pad, one Start tap 100 ms after the cut closes the iDroid, the chord fires at 1 s
		GateRun r;
		r.Step(false, true, M::kSnake, 435);
		CHECK(r.toGuest == 1 && r.guestStart);
		r.Step(false, true, M::kGuestMenu, 115);  // MGSV's menu is up; View still held
		r.Clear();
		r.Step(true, true, M::kGuestMenu, 95);
		CHECK(r.cancels == 1 && r.closes == 0 && r.maskStart && r.guestRealStartMs == 0 && r.guestStartMs == 0);
		r.Step(true, true, M::kGuestMenu, 150);
		CHECK(r.closes == 1 && r.guestStartMs >= 115 && r.guestStartMs <= 125 && r.guestRealStartMs == 0);
		r.Step(true, true, M::kSnake, 800);  // the iDroid closed
		CHECK(r.chords == 1 && r.closes == 1 && !r.sawErStart && !r.sawErView && r.presses == 0 && r.guestRealStartMs == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && !r.sawErView && r.presses == 0 && !r.gate.Holding());
	}
	{  // ...and should MGSV's menu still be open 700 ms after the first tap, a second one; never a third
		GateRun r;
		r.Step(false, true, M::kSnake, 450);
		r.Step(false, true, M::kGuestMenu, 100);
		r.Clear();
		r.Step(true, true, M::kGuestMenu, 2000);
		CHECK(r.cancels == 1 && r.closes == 2 && r.chords == 1 && r.guestRealStartMs == 0);
		r.Step(true, true, M::kGuestMenu, 2000);
		CHECK(r.closes == 2 && r.chords == 1);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && r.presses == 0);
	}
	{  // the iDroid opened by View held on, Start pressed 1.5 s after View to close it: MGSV's Start, no chord
		GateRun r;
		r.Step(false, true, M::kSnake, 450);
		r.Step(false, true, M::kGuestMenu, 1050);
		r.Clear();
		r.Step(true, true, M::kGuestMenu, 200);
		CHECK(r.cancels == 0 && r.chords == 0 && !r.maskStart && r.guestRealStartMs == 200 && r.closes == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && !r.sawErView && r.presses == 0);
	}
	{  // MGSV's menu opened by a plain iDroid hold: a Start there is MGSV's (it closes the iDroid), never masked
		GateRun r;
		r.Step(false, true, M::kSnake, 600);
		r.Step(false, false, M::kGuestMenu, 200);
		r.Step(true, false, M::kGuestMenu, 100);
		CHECK(!r.maskStart && r.closes == 0 && r.chords == 0);
		r.Step(false, false, M::kSnake, 300);
		CHECK(!r.sawErStart && r.presses == 0);
	}
	{  // Start already passed on (its menu opening), then View: no chord -- that is menu use
		GateRun r;
		r.Step(true, false, M::kSnake, 450);
		CHECK(r.erStart && r.presses == 1);
		r.Step(true, true, M::kSnake, 1500);
		CHECK(r.chords == 0);
	}
	{  // let go before 1 s: nothing at all
		GateRun r;
		r.Step(true, true, M::kSnake, 500);
		r.Step(false, false, M::kSnake, 400);
		CHECK(r.chords == 0 && !r.sawErStart && !r.sawErView && r.presses == 0 && r.toGuest == 0);
	}
	{  // Tarnished mode: Start and View both reach Elden Ring after the wait or at the release (no iDroid); the chord gives Snake
	   // back, and its buttons stay out while the mode changes under them (kSnake once the puppet runs, kOff for a frame before)
		GateRun r;
		r.Step(false, true, M::kTarnished, 600);
		CHECK(r.erView && r.toGuest == 0 && r.presses == 1);
		r.Step(false, false, M::kTarnished, 200);
		r.Step(true, false, M::kTarnished, 60);
		r.Step(false, false, M::kTarnished, 200);
		CHECK(r.sawErStart && r.presses == 2);
		r.Clear();
		r.Step(true, true, M::kTarnished, 1100);
		CHECK(r.chords == 1);
		r.Step(true, true, M::kOff, 50);
		r.Step(true, true, M::kSnake, 500);
		r.Step(false, false, M::kSnake, 300);
		CHECK(r.chords == 1 && !r.sawErStart && !r.sawErView && r.presses == 0);
	}
	{  // no chord outside kSnake/kTarnished: an Elden Ring menu (kOff) gets both as they are, counted as presses
		GateRun r;
		r.Step(true, true, M::kOff, 1500);
		CHECK(r.chords == 0 && r.erStart && r.erView && r.presses == 2);
		r.Step(true, true, M::kSnake, 200);  // the menu closed with both still held: they stay as they were
		CHECK(r.erStart && r.erView && r.chords == 0);
		r.Step(false, false, M::kSnake, 200);
	}
	{  // MGSV's menu: a Start that closes it never reaches Elden Ring, even when it is still held after the menu closed
		GateRun r;
		r.Step(true, false, M::kGuestMenu, 60);
		r.Step(true, false, M::kSnake, 400);
		r.Step(false, false, M::kSnake, 200);
		CHECK(!r.sawErStart && r.presses == 0);
	}
	{  // a press still undecided when a menu comes up (kOff) reaches no one
		GateRun r;
		r.Step(true, false, M::kSnake, 50);
		r.Step(true, false, M::kOff, 300);
		r.Step(false, false, M::kOff, 200);
		CHECK(!r.sawErStart && r.presses == 0);
	}
	{  // chord_ms 0: no chord and no wait (the old behaviour: Start at once)
		GateRun r;
		r.params.chordMs = 0;
		const auto down = r.t;
		r.Step(true, true, M::kSnake, 1500);
		CHECK(r.chords == 0 && r.erStart && r.firstErStart == down && r.toGuest == 1);
	}
}

// The test virtual pad (er/VirtualPad.h, console `input virtualpad on|exclusive|shared|off`): exclusive hides every real pad
// (2026-10-31: the player's DualSense, its stick resting pushed, walked Snake through a scripted run); shared and off leave a
// real one as it is (a player's [Keyboard] virtual_pad is shared).
static void TestVirtualPad()
{
	using V = VirtualPadMode;
	// Exclusive: user 0 is the neutral virtual pad whether a real one is there or not; users 1-3 report nothing.
	CHECK(VirtualPadPoll(V::kExclusive, 0, true) == PadPoll::kVirtual);
	CHECK(VirtualPadPoll(V::kExclusive, 0, false) == PadPoll::kVirtual);
	for (std::uint32_t i = 1; i < 4; ++i) {
		CHECK(VirtualPadPoll(V::kExclusive, i, true) == PadPoll::kDisconnected);
		CHECK(VirtualPadPoll(V::kExclusive, i, false) == PadPoll::kDisconnected);
	}
	// Shared: only an empty user 0; a real pad (on 0 or elsewhere) as it is.
	CHECK(VirtualPadPoll(V::kShared, 0, false) == PadPoll::kVirtual);
	CHECK(VirtualPadPoll(V::kShared, 0, true) == PadPoll::kReal);
	CHECK(VirtualPadPoll(V::kShared, 1, true) == PadPoll::kReal && VirtualPadPoll(V::kShared, 2, false) == PadPoll::kReal);
	// Off: everything real.
	for (std::uint32_t i = 0; i < 4; ++i) {
		CHECK(VirtualPadPoll(V::kOff, i, true) == PadPoll::kReal && VirtualPadPoll(V::kOff, i, false) == PadPoll::kReal);
	}
	CHECK(std::string(VirtualPadName(V::kExclusive)) == "exclusive" && std::string(VirtualPadName(V::kShared)) == "shared" &&
	      std::string(VirtualPadName(V::kOff)) == "off");
}

// The fall watchdog (roadmap 1.3): a chasm with no ground at all past z = 20 (a kill volume would end it in Elden Ring;
// under NoDead nothing does). The ledge fall into the synthetic world's 5 m pit lands and never fires it.
static void TestFallWatch()
{
	KinematicParams params;
	const float     dt = 1.0f / 60.0f;
	const RayFn     chasm = [](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
        const Vec3 to = a_from + a_delta;
        if (to.z > 20.0f && a_from.z > 20.0f) {
            return false;  // nothing below, ever
        }
        return Ray(a_from, a_delta, a_hit);
	};
	{  // off the edge: fires once, when the fall has gone 60 m (~3.5 s), never again
		KinematicState s;
		s.pos = { 0, 0, 19.6f };
		s.grounded = true;
		FallWatch w;
		int   fired = 0;
		float firedAt = 0;
		for (int i = 0; i < 600; ++i) {
			KinematicStep(s, { 0, 0, i < 30 ? 0.05f : 0.0f }, dt, params, chasm);
			if (w.Update(s.grounded, s.pos.y, dt, 4.0f, 60.0f)) {
				++fired;
				firedAt = static_cast<float>(i) * dt;
			}
		}
		CHECK(fired == 1 && firedAt > 3.0f && firedAt < 4.0f && s.pos.y < -60.0f);
	}
	{  // seconds only (drop check off): 4 s
		KinematicState s;
		s.pos = { 0, 0, 19.6f };
		s.grounded = true;
		FallWatch w;
		int   fired = 0;
		float firedAt = 0;
		for (int i = 0; i < 600; ++i) {
			KinematicStep(s, { 0, 0, i < 30 ? 0.05f : 0.0f }, dt, params, chasm);
			if (w.Update(s.grounded, s.pos.y, dt, 4.0f, 0.0f)) {
				++fired;
				firedAt = static_cast<float>(i) * dt;
			}
		}
		CHECK(fired == 1 && firedAt > 4.0f && firedAt < 4.6f);
	}
	{  // the 5 m pit: lands after ~1 s, re-armed, nothing fired; standing about never fires either
		KinematicState s;
		s.pos = { 0, 0, 19.5f };
		s.grounded = true;
		FallWatch w;
		int fired = 0;
		for (int i = 0; i < 600; ++i) {
			KinematicStep(s, { 0, 0, i < 60 ? 0.05f : 0.0f }, dt, params, &Ray);
			fired += w.Update(s.grounded, s.pos.y, dt, 4.0f, 60.0f) ? 1 : 0;
		}
		CHECK(fired == 0 && s.grounded && Near(s.pos.y, -5.0f, 1e-2f) && w.Seconds() == 0.0f);
	}
	{  // an origin shift mid-fall is no drop
		FallWatch w;
		CHECK(!w.Update(false, 100.0f, 0.1f, 0.0f, 60.0f));
		w.Shift(-80.0f);
		CHECK(!w.Update(false, 15.0f, 0.1f, 0.0f, 60.0f) && Near(w.Drop(), 5.0f, 1e-3f));
		CHECK(w.Update(false, -50.0f, 0.1f, 0.0f, 60.0f));
		CHECK(!w.Update(true, -50.0f, 0.1f, 0.0f, 60.0f) && w.Drop() == 0.0f);
	}
}

// Ladders (er/Ladder.h): Snake's press on "Climb" hands the Tarnished to Elden Ring until he is off the ladder.
static void TestLadder()
{
	using phantom::er::LadderHandOver;
	using Ev = LadderHandOver::Event;
	// The climb over for good (MGSV told then, so it has Snake home before the settle ends): none after EndTop / EndBottom only.
	CHECK(phantom::er::LadderEndedForGood(6, -1) && phantom::er::LadderEndedForGood(7, -1));
	CHECK(!phantom::er::LadderEndedForGood(2, -1) && !phantom::er::LadderEndedForGood(9, -1) && !phantom::er::LadderEndedForGood(14, -1));  // a flicker
	CHECK(!phantom::er::LadderEndedForGood(6, 6) && !phantom::er::LadderEndedForGood(-1, -1) && !phantom::er::LadderEndedForGood(6, 8));
	LadderHandOver::Input in;
	in.nowMs = 1000;
	in.snake = true;
	{  // a climb: press -> waiting -> on it (StartBottom .. EndTop) -> off -> 300 ms later Snake again
		LadderHandOver l;
		CHECK(l.Update(in) == Ev::kNone && !l.Holding());
		in.pressed = true;
		CHECK(l.Update(in) == Ev::kStarted && l.Holding() && l.Current() == LadderHandOver::State::kWaiting);
		CHECK(!l.PressDue(in.nowMs, 100));  // the puppet steps aside first
		in.pressed = false;
		in.snake = false;  // the puppet stepped aside
		in.nowMs += 100;
		CHECK(l.Update(in) == Ev::kNone && l.Holding());
		CHECK(l.PressDue(in.nowMs, 100) && !l.PressDue(in.nowMs + 50, 100));  // Elden Ring gets the press once
		in.onLadder = true;
		in.nowMs += 200;
		CHECK(l.Update(in) == Ev::kClimbing && l.Current() == LadderHandOver::State::kClimbing);
		in.nowMs += 9000;  // a long climb: never times out
		CHECK(l.Update(in) == Ev::kNone && l.Holding());
		in.onLadder = false;  // one frame of "none" between two of its states
		in.nowMs += 16;
		CHECK(l.Update(in) == Ev::kNone && l.Current() == LadderHandOver::State::kSettling);
		in.onLadder = true;
		in.nowMs += 16;
		CHECK(l.Update(in) == Ev::kNone && l.Current() == LadderHandOver::State::kClimbing);
		in.onLadder = false;  // off at the top
		in.nowMs += 16;
		CHECK(l.Update(in) == Ev::kNone && l.Holding());
		in.nowMs += 200;
		CHECK(l.Update(in) == Ev::kNone && l.Holding());
		in.nowMs += 100;
		CHECK(l.Update(in) == Ev::kEnded && !l.Holding());
		in.snake = true;
		in.nowMs += 16;
		CHECK(l.Update(in) == Ev::kNone);  // Snake plays on: nothing until the next ladder
	}
	{  // a press Elden Ring never turns into a climb (out of reach, grayed out): Snake plays on after ladder_wait_ms
		LadderHandOver l;
		in = {};
		in.nowMs = 1000;
		in.snake = true;
		in.pressed = true;
		CHECK(l.Update(in) == Ev::kStarted);
		in.pressed = false;
		in.snake = false;
		in.nowMs += 1499;
		CHECK(l.Update(in) == Ev::kNone && l.Holding());
		in.nowMs += 1;
		CHECK(l.Update(in) == Ev::kNoClimb && !l.Holding());
	}
	{  // a ladder state while Snake plays (a press we didn't see: the keyboard): the hand-over starts climbing
		LadderHandOver l;
		in = {};
		in.nowMs = 1000;
		in.snake = true;
		in.onLadder = true;
		CHECK(l.Update(in) == Ev::kStarted && l.Current() == LadderHandOver::State::kClimbing);
		CHECK(!l.PressDue(in.nowMs + 500, 100));  // no press of ours to give: already climbing
		// ...and not while the Tarnished has the controller anyway (Tarnished mode climbs as the player likes)
		LadderHandOver idle;
		in.snake = false;
		CHECK(idle.Update(in) == Ev::kNone && !idle.Holding());
	}
	{  // a death or a load on the way lets go at once
		LadderHandOver l;
		in = {};
		in.nowMs = 1000;
		in.snake = true;
		in.pressed = true;
		CHECK(l.Update(in) == Ev::kStarted);
		in.pressed = false;
		in.onLadder = true;
		in.nowMs += 300;
		CHECK(l.Update(in) == Ev::kClimbing);
		in.inWorld = false;
		in.nowMs += 16;
		CHECK(l.Update(in) == Ev::kAborted && !l.Holding());
		CHECK(l.Update(in) == Ev::kNone);
	}
	CHECK(phantom::er::LadderStateName(-1) == "none" && phantom::er::LadderStateName(0) == "StartBottom" && phantom::er::LadderStateName(6) == "EndTop");
}

// Snake climbs the ladder himself (protocol v15 kHEvClimb, er/Ladder.h ClimbPace): when the guest hears of the climb, its turns and
// its pace; the end once.
static void TestClimbPace()
{
	using phantom::er::ClimbPace;
	using phantom::er::LadderMotion;
	CHECK(LadderMotion(0) == 1 && LadderMotion(2) == 1 && LadderMotion(3) == 1 && LadderMotion(6) == 1);
	CHECK(LadderMotion(1) == -1 && LadderMotion(4) == -1 && LadderMotion(5) == -1 && LadderMotion(7) == -1 && LadderMotion(15) == -1);
	CHECK(LadderMotion(8) == 0 && LadderMotion(9) == 0 && LadderMotion(-1) == 0 && LadderMotion(21) == 0 && LadderMotion(99) == 0);
	{  // up a 19 m ladder at Elden Ring's ~1.1 m/s, a rest on the rungs half way, then down again, off at the bottom
		ClimbPace p;
		std::uint64_t now = 1000;
		float         y = 0.0f;
		auto o = p.Update(now, 0, y, 1.2f);  // StartBottom: the climb starts, going up, at the default pace (nothing measured yet)
		CHECK(o.send && o.on && !o.down && Near(o.speed, 1.2f) && o.motion == 1 && p.Started());
		int sends = 0;
		for (int i = 0; i < 300; ++i) {  // 5 s up at 1.1 m/s, rung states alternating
			now += 16;
			y += 1.1f * 0.016f;
			o = p.Update(now, (i / 40) % 2 ? 3 : 2, y, 1.2f);
			sends += o.send ? 1 : 0;
		}
		CHECK(sends == 0);  // 1.1 vs 1.2 m/s told: within the pace change; Up left / Up right is no turn
		CHECK(Near(p.Speed(), 1.1f, 0.08f));
		for (int i = 0; i < 12; ++i) {  // IdleLeft for 0.19 s between two pushes of the stick: no stop as far as MGSV's climb goes
			now += 16;
			o = p.Update(now, 9, y, 1.2f);
			CHECK(!o.send);
		}
		now += 16;
		o = p.Update(now, 3, y, 1.2f);  // up again
		CHECK(!o.send);
		sends = 0;
		for (int i = 0; i < 25 && !o.send; ++i) {  // a real stop on the rungs: told once it lasted kStillMs
			now += 16;
			o = p.Update(now, 9, y, 1.2f);
		}
		CHECK(o.send && o.on && !o.down && o.speed == 0.0f && o.motion == 0);
		now += 50;
		o = p.Update(now, 3, y, 1.2f);  // up again 50 ms later: within the gap -> waits
		CHECK(!o.send);
		now += 50;
		o = p.Update(now, 9, y, 1.2f);  // ...and still again before the gap is over: nothing to tell (no turn after all)
		CHECK(!o.send);
		for (int i = 0; i < 30; ++i) {  // idle a while
			now += 16;
			o = p.Update(now, 9, y, 1.2f);
			CHECK(!o.send);
		}
		now += 16;
		o = p.Update(now, 4, y, 1.2f);  // DownRight: down now, at the last climbing pace until a measure is warm
		CHECK(o.send && o.on && o.down && o.speed > 1.0f && o.speed < 1.25f && o.motion == -1);
		sends = 0;
		for (int i = 0; i < 120; ++i) {  // sliding down (the coast) at 6 m/s: the pace changes -> told once it is measured
			now += 16;
			y -= 6.0f * 0.016f;
			o = p.Update(now, 15, y, 1.2f);
			if (o.send) {
				++sends;
				CHECK(o.down && o.speed > 3.0f && o.motion == -1);
			}
		}
		CHECK(sends >= 1 && sends <= 3);
		now += 16;
		o = p.Update(now, -1, y, 1.2f);  // a frame of "none" between two states: nothing
		CHECK(!o.send);
		o = p.End();
		CHECK(o.send && !o.on && o.down && !p.Started());
		o = p.End();  // once
		CHECK(!o.send);
	}
	{  // a hand-over without any climb (Elden Ring refused the press): nothing to end
		ClimbPace p;
		CHECK(!p.End().send);
	}
	{  // the first state is "down" (StartTop at the top): b = 1 from the first event
		ClimbPace p;
		const auto o = p.Update(5000, 1, 19.0f, 1.2f);
		CHECK(o.send && o.on && o.down);
	}
	{  // no default speed to tell: a moving climb sends "unknown" (-1, MGSV's own pace), never 0 -- which is "still on the rungs"
		// (review 2026-10-31: the guest read 0 as unknown and climbed on while the Tarnished hung still)
		ClimbPace p;
		auto o = p.Update(1000, 0, 0.0f, 0.0f);
		CHECK(o.send && o.motion == 1 && o.speed < 0.0f);
		ClimbPace still;
		o = still.Update(1000, 8, 0.0f, 1.2f);  // the first state an idle one: still on the rungs
		CHECK(o.send && o.motion == 0 && o.speed == 0.0f);
	}
	{  // an origin shift mid-climb (live: 32 m at Fort Haight's tower) is no speed
		ClimbPace p;
		std::uint64_t now = 1000;
		float         y = 0.0f;
		p.Update(now, 0, y, 1.2f);
		for (int i = 0; i < 120; ++i) {
			now += 16;
			y += 1.1f * 0.016f;
			p.Update(now, (i / 40) % 2 ? 3 : 2, y, 1.2f);
		}
		const float before = p.Speed();
		y -= 32.0f;
		p.Shift(-32.0f);
		now += 16;
		y += 1.1f * 0.016f;
		const auto o = p.Update(now, 2, y, 1.2f);
		CHECK(!o.send && Near(p.Speed(), before, 0.05f));
		ClimbPace unshifted;  // the same without Shift: one frame of 2000 m/s, clamped to 20 and eased in
		unshifted.Update(1000, 0, 0.0f, 1.2f);
		unshifted.Update(1016, 2, 0.02f, 1.2f);
		unshifted.Update(1032, 2, -32.0f, 1.2f);
		CHECK(unshifted.Speed() > 0.3f);
	}
}

// The puppet's start on a rim (er/Kinematic.h SupportedStart): live, off a ladder the Tarnished's centre was over the shaft.
static void TestSupportedStart()
{
	KinematicParams params;
	CHECK(!SupportedStart({ 0, 0, 5 }, params, Ray));  // ground under the centre: start there
	// The pit begins at z > 20. 0.1 m past its edge two of BodyGround's side probes (er/Kinematic.h) are on the rim -- enough to
	// hold a walking body, not a start: 4 cm more toward the pit and he'd fall (review 2026-10-14). Moved onto the ground: the
	// whole body (centre 0.35 m in), or all five probes at its level.
	const float top = params.stepUp + 0.05f;
	const auto  startOk = [&](const Vec3& a_at) { return StartGround(BodyGround(a_at, top, top + params.snapDown, params.radius, Ray)); };
	CHECK(BodyGround({ 0, 0, 20.1f }, top, top + params.snapDown, params.radius, Ray).supported && !startOk({ 0, 0, 20.1f }));
	const auto rim = SupportedStart({ 0, 0, 20.1f }, params, Ray);
	CHECK(rim && (rim->z <= 20.0f - params.radius || (rim->z <= 19.81f && startOk(*rim))) && rim->z > 19.2f && Near(rim->y, 0.0f, 0.01f));
	// the nearest supported point, never one through the wall at x 5..5.5
	const auto wall = SupportedStart({ 4.9f, 0, 20.1f }, params, Ray);
	CHECK(wall && wall->x < 5.0f && wall->z <= 20.0f);
	// Any start with the centre over the pit (1-29 cm past the edge), whatever way the world is turned (the probes' cross is fixed
	// in world axes): moved about 0.2 m or more in from the edge, onto start ground.
	for (const float deg : { 0.0f, 15.0f, 30.0f, 45.0f, 60.0f, 75.0f }) {
		const float a = deg * std::numbers::pi_v<float> / 180.0f, ca = std::cos(a), sa = std::sin(a);
		const auto  toWorld = [=](const Vec3& v) { return Vec3{ v.x * ca - v.z * sa, v.y, v.x * sa + v.z * ca }; };
		const auto  toBase = [=](const Vec3& v) { return Vec3{ v.x * ca + v.z * sa, v.y, -v.x * sa + v.z * ca }; };
		const RayFn turned = [&](const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit) {
			Vec3 h;
			if (!Ray(toBase(a_from), toBase(a_delta), h)) {
				return false;
			}
			a_hit = toWorld(h);
			return true;
		};
		int bad = 0;
		for (float d = 0.01f; d < 0.3f; d += 0.02f) {
			const auto at = SupportedStart(toWorld({ 0, 0, 20.0f + d }), params, turned);
			bad += at && toBase(*at).z <= 19.81f && Near(at->y, 0.0f, 0.01f) &&
			               StartGround(BodyGround(*at, top, top + params.snapDown, params.radius, turned))
			           ? 0
			           : 1;
		}
		CHECK(bad == 0);
	}
	CHECK(!SupportedStart({ 0, 0, 23.0f }, params, Ray));  // deep in the pit: nothing within 1.2 m -- a real drop
	// kinematic from that start stays up
	KinematicState s;
	s.pos = *rim;
	s.grounded = true;
	KinematicStep(s, {}, 1.0f / 60.0f, params, Ray);
	CHECK(s.grounded && Near(s.pos.y, 0.0f, 0.01f));
}
