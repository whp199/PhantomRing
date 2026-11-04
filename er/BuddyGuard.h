#pragma once

// D-Dog's place and drawn body, guarded (0.8.0 polish; docs/HOST.md "D-Dog in Elden Ring"; tests/test_host_dog.inc). Pure: no game
// access, the caller (er/Buddy.cpp for its placement, er/RenderScene.cpp for its draws) feeds it one point a frame.
//
// The 0.8.0 integration run measured the dog's MGSV place as Elden Ring maps it (10 minutes, 16 orders): 9 steps of 0.5-0.7 m in one
// frame while it bit (MGSV's leap snapping its root onto the target), once a 5.9 m snap, twice a one-frame 6-8 m blip, 0.52 / 0.58 m
// outside a bite, and once ~77 m for 0.33 s right after a climb-up (MGSV's crate / drum site). The guest fixes what it can at the
// source; this is the host's backstop, and it logs each time it acts so it never hides a guest-side fault from a test:
//  - StepGuard: a jump of the point that comes back is held (the point stays where it was: never drawn there); a real move that
//    stays (a leash return, a re-anchor) is taken at once where neither the old nor the new place is in view, eased in at most
//    maxStep a frame when it is short (after holdShortMs), and a far one taken at the latest after holdInViewMs; a snap (a step far off the point's own speed but
//    short of a jump) is spread over a few frames (the excess decays with settleS). The drawn point never moves more than maxStep
//    in a 60 Hz frame except where it is taken out of view (or, a real far move, after holdInViewMs).
//  - WaitPin: a waiting D-Dog keeps its MGSV place (the guest moves it with Snake's treadmill jumps); where Elden Ring's walls stopped
//    Snake on his way MGSV's Snake went on, and the anchor (Snake's two places) carried the dog's Elden Ring spot off by that much
//    (live: 3 m at the courtyard's barricades). The pin takes back the part of the dog's drift that Snake's slip since the Wait --
//    his Elden Ring step minus his MGSV step (the odometer) mapped to Elden Ring -- explains, so the dog stays on its Elden Ring spot
//    whether or not the guest moves MGSV's dog by the slip too; it is let go of (eased) once the dog moves on its own (the wait
//    over), at the player's whistle / DD, or when MGSV takes an attack order (the dog runs from its Wait; eased with its run).

#include "Math.h"

#include <cstdint>

namespace phantom::er::buddyguard
{
	struct Rules
	{
		float    maxStep{ 0.5f };        // m the guarded point may move in a 60 Hz frame (scaled with longer frames)
		float    snapTol{ 0.2f };        // m off the point's own speed in one frame: a snap (spread over settleS)
		float    jumpMin{ 1.5f };        // m off its own speed in one frame: a jump (held until it comes back or proves real)
		float    settleS{ 0.1f };        // s: the time constant of a snap's / a return's leftover
		float    speedMax{ 15.0f };      // m/s: the fastest the buddy itself moves (jumpMin grows by this x a long frame's extra time)
		std::uint32_t holdShortMs{ 100 };  // a held jump within easeMax that hasn't come back by then is a real move: eased in
		std::uint32_t holdMs{ 500 };     // a longer one (or, for the placement, any) that hasn't come back by then is a real move
		std::uint32_t holdInViewMs{ 2000 };  // ...one whose either end is in view is held up to this long (it may leave the view)
		float    easeMax{ 10.0f };       // m: a real move up to this long in view is eased in (maxStep a frame); longer ones are taken
	};
	// The placement's rules (er/Buddy.cpp: no view; a real move is taken after holdMs, never eased).
	inline Rules PlacementRules()
	{
		Rules r;
		r.holdInViewMs = r.holdMs;
		r.holdShortMs = r.holdMs;
		r.easeMax = 0.0f;
		return r;
	}

	enum class Act : std::uint8_t
	{
		kNone,
		kSnap,        // a snap: its excess spread over the next frames
		kHold,        // a jump held (the point stays)
		kHolding,     // still held
		kReturned,    // the jump came back: it was never shown
		kEase,        // a real move, eased in
		kTakenOut,    // a real move (or a jump) taken at once: neither end in view
		kTakenLate,   // a real far move taken in view after holdInViewMs
		kLimited,     // the leftover's decay was held to maxStep
		kCount
	};
	const char* Name(Act a_act);

	struct Step
	{
		Vec3  offset;           // add to the raw point (and to everything drawn with it) this frame
		Act   act{ Act::kNone };
		float raw{ 0 };         // m the raw point moved this frame
		float drawn{ 0 };       // m the guarded point moved this frame
		float jump{ 0 };        // kHold / kReturned / kEase / kTaken*: how far the raw point was off (m)
		std::uint32_t heldMs{ 0 };  // kReturned / kEase / kTaken*: how long it was held
	};

	class StepGuard
	{
	public:
		// m: a held jump counts as come back within max(1 m, half the jump) of the held point, but never further than this (review
		// 0.8.0: a 60 m jump settling 20 m off was "came back" and glided in view).
		static constexpr float kReturnMax = 2.0f;
		// One frame: a_raw = the point as mapped (Elden Ring space); a_rawInView / a_drawnInView: the camera sees the raw point / the
		// one drawn last frame (pass true when unknown: then nothing is taken at once).
		Step Update(const Vec3& a_raw, bool a_rawInView, bool a_drawnInView, float a_dt, std::uint64_t a_nowMs, const Rules& a_rules);
		void Reset() { *this = {}; }
		// Elden Ring moved its world origin (er/WorldShift.h): every place kept moves with it, nothing jumps.
		void Shift(const Vec3& a_delta)
		{
			prevRaw_ = prevRaw_ + a_delta;
			drawn_ = drawn_ + a_delta;
			heldAt_ = heldAt_ + a_delta;
		}
		[[nodiscard]] bool Holding() const { return holding_; }
		[[nodiscard]] Vec3 Offset() const { return offset_; }
		[[nodiscard]] Vec3 Drawn() const { return drawn_; }

	private:
		bool          have_{ false };
		Vec3          prevRaw_, drawn_, offset_;
		Vec3          hist_[3];          // its last three steps (a jump's left out): their median is its own motion
		int           next_{ 0 };
		int           steps_{ 0 };
		float         histDt_{ 1.0f / 60.0f };
		bool          holding_{ false };
		std::uint64_t holdSince_{ 0 };
		Vec3          heldAt_;
		float         heldJump_{ 0 };
		bool          easing_{ false };
	};

	class WaitPin
	{
	public:
		// The player's Wait (9): from now on Snake's slip counts; a_dogEr = the dog's Elden Ring place as mapped now (no correction).
		void Begin(const Vec3& a_dogEr);
		// The wait is over (the whistle, DD, an attack order MGSV took, the dog moving in MGSV): the correction eases back to 0, at
		// kEaseSpeed or with the dog's own run (kFollowShare of its step), whichever is faster.
		void End();
		void Reset() { *this = {}; }
		void Shift(const Vec3& a_delta)  // Elden Ring moved its world origin (er/WorldShift.h)
		{
			at_ = at_ + a_delta;
			lastDog_ = lastDog_ + a_delta;
		}
		// One frame. a_slip: Snake's Elden Ring step minus his MGSV step mapped to Elden Ring (horizontal, m); a_dogEr: the dog's place
		// as mapped this frame (no correction); a_staged: MGSV plays a staged move (its places then mean nothing: the correction is
		// kept as it is until kAfterStagedS after it). Returns the Elden Ring shift to add to the mapped dog (horizontal).
		//
		// Only what Snake's slip explains of the dog's drift is taken back: the drift's part along the slip, at most the slip itself.
		// So a guest that keeps the waiting dog's place itself (PhantomRingGuest.ini [Dog] WaitKeepsPlace: MGSV's dog moved by the
		// slip, the drift ~0) leaves nothing to correct here -- the two never add up -- and while its correction lags the host fills
		// the gap. The dog's own moves off that line pass through; over kMovedEnds m of them the wait is over.
		Vec3 Update(const Vec3& a_slip, const Vec3& a_dogEr, bool a_staged, float a_dt);
		[[nodiscard]] bool  Waiting() const { return waiting_; }
		[[nodiscard]] Vec3  Shift() const { return shift_; }
		[[nodiscard]] Vec3  Slip() const { return slip_; }     // Snake's slip since the Wait (Elden Ring, horizontal)
		[[nodiscard]] float Moved() const { return moved_; }  // m the dog moved off the slip's line since the wait began
		// Why the last wait ended on its own (nullptr: not yet / by End).
		[[nodiscard]] const char* EndedBy() const { return endedBy_; }

		static constexpr float kMovedEnds = 1.5f;     // m the dog moves that the slip doesn't explain: the wait is over
		static constexpr float kEaseSpeed = 2.0f;     // m/s the correction eases back once it is (at least)...
		static constexpr float kFollowShare = 0.75f;  // ...or this share of the dog's own step a frame, while it runs (an order's run)
		static constexpr float kRunStepMax = 2.0f;    // m: a step of the mapped dog beyond this is a jump, not its run
		// m: Snake held against Elden Ring's wall this long during a Wait (the slip only builds up frame by frame: a teleport is dropped
		// by Buddy's Slip()) -- the pin lets go, and the correction eases out (never dropped in one frame)
		static constexpr float kMax = 40.0f;
		static constexpr float kAfterStagedS = 1.0f;  // s after a staged move during which the dog's mapped place isn't trusted

	private:
		bool        waiting_{ false };
		Vec3        at_;         // where the dog was mapped at the Wait
		Vec3        slip_;       // Snake's slip since
		Vec3        shift_;
		float       moved_{ 0 };
		float       afterStaged_{ 0 };
		const char* endedBy_{ nullptr };
		Vec3        lastDog_;    // the mapped dog last frame (its run eases the correction out)
		bool        haveDog_{ false };
	};
}
