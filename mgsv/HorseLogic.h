#pragma once
// D-Horse treadmill, the pure half (docs/GUEST.md 8.1a; game glue in Horse.cpp, tests in tests/test_guest_horse.cpp).
//
// TppHorse2 answers no SetPosition/Warp (live 2026-10-13), and the player location manager knows only the player family
// (`!loc 0x1a00`: none). ActorProxy v1 found the way for soldiers (GUEST.md 20.5): every realized character owns a location
// object of the class Snake's belongs to (vtable 0x1424C8EF0, built by 0x141ACED60 at +0x10 of a 0x3F0-byte body), and that
// object's own setter (vf 0x68) moves him. The horse is a realized character too, so its object is looked for the same way:
// among the scanned location objects, the one whose feet (+0x10) stand on the horse's skin root (Skeleton.cpp) and keep doing
// so while it moves. UNVERIFIED live: that TppHorse2 has one at all, and that Snake in the saddle follows it.

#include "Logic.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace phantom::guest::horse::plan
{
	using logic::Vec3;

	struct LocSample
	{
		std::uintptr_t loc{ 0 };
		Vec3           feet;  // loc+0x10
	};

	struct PickParams
	{
		float maxXZ{ 0.75f };  // the horse's feet vs its skin root (m, horizontal)
		float maxDy{ 1.0f };   // ... and vertically: Snake's own object, in the saddle, stands ~1.4 m above the horse's root
	};

	// The location object standing nearest the horse's skin root within the window, none of a_exclude (Snake's own, ActorProxy's
	// soldiers'), and not one of the pooled objects parked at the world origin. 0 = none.
	std::uintptr_t PickLocation(const std::vector<LocSample>& a_samples, const Vec3& a_horseRoot, const std::vector<std::uintptr_t>& a_exclude,
		const PickParams& a_params = {});

	// A candidate becomes the horse's object once its feet stayed on the skin root for kConfirmSamples checks over at least
	// kConfirmMs (a soldier's object passing by doesn't), and stops being it after kLoseSamples checks in a row off the skin
	// (the pooled object went to another character) or at once when it is no longer a location object.
	class Tracker
	{
	public:
		static constexpr int           kConfirmSamples = 10;
		static constexpr std::uint64_t kConfirmMs = 500;
		static constexpr int           kLoseSamples = 15;

		enum class Verdict : std::uint8_t
		{
			kNone,       // nothing to track
			kCandidate,  // still being confirmed
			kConfirmed,  // the horse's (this check)
			kLost,       // it was dropped by this check
		};

		explicit Tracker(const PickParams& a_params = {}) : m_params(a_params) {}

		void Reset();
		// A new candidate (the scan's pick): confirmation starts over.
		void Offer(std::uintptr_t a_loc, std::uint64_t a_nowMs);
		// One check. a_feet = the object's feet now (nullopt: not a location object any more), a_skin = the horse's skin root
		// (nullopt: not published lately -- the verdict holds).
		Verdict Observe(const std::optional<Vec3>& a_feet, const std::optional<Vec3>& a_skin, std::uint64_t a_nowMs);
		// The skin just jumped with a placement of ours: the next kSettleChecks disagreements don't count (SkinPublish runs
		// later in the frame than the placement).
		void NoteShift();

		[[nodiscard]] std::uintptr_t Loc() const { return m_loc; }
		[[nodiscard]] bool           Confirmed() const { return m_confirmed; }
		[[nodiscard]] int            Agreed() const { return m_agreed; }

	private:
		static constexpr int kSettleChecks = 10;

		PickParams     m_params;
		std::uintptr_t m_loc{ 0 };
		bool           m_confirmed{ false };
		int            m_agreed{ 0 };
		int            m_off{ 0 };
		int            m_settle{ 0 };
		std::uint64_t  m_firstAgreedMs{ 0 };
	};

	struct Placement
	{
		Vec3                 pos;  // the setter's point (what loc+0x20 holds: feet + the class's own lift)
		std::array<float, 4> rot;  // xyzw
	};

	// The setter's arguments for a treadmill shift: the point the object holds now (+0x20) moved by a_shift (all three axes), its
	// own rotation (+0x60) kept -- so the class's lift above the feet and the horse's heading come from the object itself. A
	// rotation that isn't a unit quaternion falls back to the heading a_fallbackYaw. Nothing for non-finite input.
	std::optional<Placement> Shifted(const Vec3& a_at, const std::array<float, 4>& a_rot, const Vec3& a_shift, float a_fallbackYaw);

	// A ridden jump's shift with a vertical target: the treadmill's horizontal shift, and the height that puts the horse's feet
	// (+0x10) on a_groundY -- the centre's ground (Snake's on-foot root there). The treadmill's own shift.y is for Snake's root, a
	// saddle (~1.4 m) above the horse's feet, and the ride's far point can be metres above or below the centre: keeping the
	// object's old height would bury horse and rider or drop them from the air.
	Vec3 GroundedShift(const Vec3& a_shift, const Vec3& a_feet, float a_groundY);

	// How far from the centre a ridden jump may start (m). A gallop (~15 m/s) gets ~15 m past it between jumps (1 s cooldown);
	// further out (a ride that went on without the treadmill, the object found again later) the horse is not thrown back in one
	// go -- the treadmill waits until Snake rides back within it, or dismounts (on foot the native placement takes him back).
	inline float RiddenJumpLimit(float a_radius) { return 2.0f * a_radius + 20.0f; }
	bool         RiddenJumpOk(const Vec3& a_root, const Vec3& a_centre, float a_radius);

	// After a ridden jump (the horse moved, Snake left to the saddle) his root must jump too, within kFrames frames *and* kMs ms
	// (whichever is longer, at any frame rate). If it doesn't, the saddle doesn't carry him: the horse is put back under him at
	// once and no ridden jump is tried again this session -- rather than the generic 3 s vehicle window, three times over,
	// moving the horse ~15 m away from its rider each time.
	class RiddenWatch
	{
	public:
		static constexpr std::uint64_t kFrames = 10;
		static constexpr std::uint64_t kMs = 300;

		void Start(std::uint64_t a_frame, std::uint64_t a_nowMs)
		{
			m_open = true;
			m_frame = a_frame;
			m_ms = a_nowMs;
		}
		void               Clear() { m_open = false; }
		[[nodiscard]] bool Open() const { return m_open; }
		// The window passed without Snake's landing (Clear).
		[[nodiscard]] bool Missed(std::uint64_t a_frame, std::uint64_t a_nowMs) const
		{
			return m_open && a_frame >= m_frame + kFrames && a_nowMs >= m_ms + kMs;
		}

	private:
		bool          m_open{ false };
		std::uint64_t m_frame{ 0 };
		std::uint64_t m_ms{ 0 };
	};
	// Where the object goes back to after such a miss: its point before the jump moved as Snake's root has moved since (he kept
	// riding, or stood) -- so the horse is under him again, whatever the horse itself did meanwhile.
	inline Vec3 BackUnder(const Vec3& a_atBefore, const Vec3& a_rootBefore, const Vec3& a_rootNow) { return a_atBefore + (a_rootNow - a_rootBefore); }

	// Did something at a_before end up a_shift (horizontal) further on, within a_tolerance (m)? The object's read-back right after
	// the setter, the skin a few frames later, Snake's root when he rides.
	bool Followed(const Vec3& a_before, const Vec3& a_after, const Vec3& a_shift, float a_tolerance);

	// What the treadmill does with D-Horse on a jump.
	enum class Action : std::uint8_t
	{
		kLeave,       // nothing (no horse, or its place unknown)
		kWhistle,     // today's way: it stays, MGSV's whistle brings it back to Snake (Follow.cpp)
		kMoveAlong,   // Snake on foot: the horse moved by the same jump (as D-Walker)
		kMoveRidden,  // Snake rides it: the horse moved *as* the jump, Snake in the saddle with it
		kNoTreadmill, // Snake rides it and it can't be moved: no treadmill (MGSV's own map limits the ride)
	};
	struct ActionInput
	{
		bool enabled{ false };    // [Buddy] HorseTreadmill
		bool horse{ false };      // D-Horse is the active buddy
		bool posValid{ false };   // its skin is known
		bool ridden{ false };     // Snake rides it
		bool located{ false };    // its location object is confirmed
		bool unmovable{ false };  // moving it failed kMaxFailures times this session
		bool riderLeft{ false };  // a ridden jump left Snake behind (RiddenWatch): the saddle doesn't carry him, this session
	};
	inline constexpr int kMaxFailures = 3;
	Action Decide(const ActionInput& a_in);
}
