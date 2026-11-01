#pragma once

// Snake climbs onto, or vaults over, an Elden Ring ledge in front of him (protocol v15 kHEvMantle / kGuestMantling; [Mantle]).
// MGSV has its own "climb up" (Y at a wall a metre or two high) and its vault over low fences, but MGSV's flat airport has no
// such walls where Elden Ring has them: the host finds the ledge in Elden Ring's world with its rays, tells the guest which
// move to play and for how long (kHEvMantle a = height cm, b = 0 climb up / 1 vault over, v0[0] = duration s), and carries the
// kinematic puppet along a short up-and-over path while MGSV plays it (it starts when the guest says it plays, kGuestMantling,
// or a moment later without it).
//
// Pure (tests/test_host_mantle.inc): the ray cast is a callback, as for er/Kinematic.h.

#include "Kinematic.h"
#include "MantleParams.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace phantom::er
{
	enum class MantleKind : std::uint8_t
	{
		kNone,
		kClimb,  // onto the top (b = 0)
		kVault,  // over it, down the other side (b = 1)
	};

	struct MantlePlan
	{
		MantleKind  kind{ MantleKind::kNone };
		const char* why{ "" };       // kNone: why not (for the log and the hints)
		bool        ledge{ false };  // a ledge face was found in reach (even if refused: too high, no room)
		float       height{ 0 };     // its top above the feet (m)
		float       face{ 0 };       // its face's distance from the body's axis (m)
		float       depth{ 0 };      // vault: the obstacle's depth (m)
		float       drop{ 0 };       // vault: how far the ground behind is below the feet (m, < 0 above)
		Vec3        dir;             // horizontal, unit
		float       duration{ 0 };   // s
		bool        squared{ false };  // planned square to the face (its normal), not along the asked direction (a slanted approach)
		int         farther{ 0 };      // climb: its landing is this many steps (0.25 m) past the first one (stacked rubble, a rising top)
		float       slant{ 0 };        // degrees between the asked direction and the face's normal (0 = straight on; -1 unknown)
		bool        hasBlock{ false }; // kNone: where the refusing ray hit (relative to the feet; the log and `mantle plan`)
		Vec3        block;
		std::array<Vec3, 4>  keys{};  // the feet along the path: keys[0] = the start ...
		std::array<float, 4> keyT{};  // ... at these times (s; keyT[0] = 0, the last = duration)
		int                  keyCount{ 0 };
		[[nodiscard]] Vec3   End() const { return keyCount ? keys[static_cast<std::size_t>(keyCount - 1)] : Vec3{}; }
	};

	// What Snake standing at a_feet facing a_dir (horizontal) can do with the obstacle in front of him. a_body: his height in
	// his stance (room to stand where he ends up); a_params' radius is his body's. a_vault: a thin obstacle is vaulted over when
	// it can be (Y and running into it both do: MGSV vaults a fence); false = only climb onto things (the vault stays possible
	// only where climbing isn't: nothing to stand on).
	//
	// A face met at a slant (live 2026-10-31: running into a ledge at a slant slid along it) is planned square to it as well, its
	// normal from three rays across the face, when the straight plan along a_dir is refused and a_dir is at most a_maxSlantDeg from
	// the face's normal (0 = never square up): MGSV climbs and vaults straight on.
	MantlePlan PlanMantle(const Vec3& a_feet, const Vec3& a_dir, float a_body, const MantleParams& a_m, const KinematicParams& a_params, const RayFn& a_ray,
		bool a_vault = true, float a_maxSlantDeg = 0.0f);

	// The face straight ahead (its nearest point within a_max of the body's axis, along a_dir) and its horizontal normal (unit, toward
	// a_feet) from three parallel rays a_spread apart at the height that hit nearest. nullopt: no face, or no normal.
	std::optional<Vec3> FaceNormal(const Vec3& a_feet, const Vec3& a_dir, float a_max, float a_spread, const RayFn& a_ray);

	// The feet a_t seconds into the plan (clamped): each leg eased in and out.
	Vec3 MantlePoint(const MantlePlan& a_plan, float a_t);

	std::string_view Name(MantleKind a_kind);

	// The move as it runs (er/Puppet.cpp): asked (kHEvMantle sent), waiting for the guest's kGuestMantling (at most a_waitMs), then
	// moving along the plan; done once its time is up.
	class MantleRun
	{
	public:
		enum class Phase : std::uint8_t
		{
			kIdle,
			kWaiting,  // kHEvMantle went out: Snake holds still until MGSV plays the move (kGuestMantling) or a_waitMs passed
			kMoving,
		};
		void Begin(const MantlePlan& a_plan, std::uint64_t a_nowMs)
		{
			plan_ = a_plan;
			phase_ = Phase::kWaiting;
			askedMs_ = a_nowMs;
			guest_ = false;
		}
		// Each frame: whether MGSV plays it. Returns the feet (nullopt: idle). a_done is set on the frame it ends.
		std::optional<Vec3> Update(std::uint64_t a_nowMs, bool a_guestMantling, std::uint64_t a_waitMs, bool* a_done = nullptr);
		void                Abort() { phase_ = Phase::kIdle; }
		[[nodiscard]] Phase              Current() const { return phase_; }
		[[nodiscard]] bool               Active() const { return phase_ != Phase::kIdle; }
		[[nodiscard]] const MantlePlan&  Plan() const { return plan_; }
		[[nodiscard]] bool               GuestPlayed() const { return guest_; }  // the move began with kGuestMantling (not the timeout)
		[[nodiscard]] std::uint64_t      WaitedMs() const { return startMs_ - askedMs_; }
		void                             Shift(const Vec3& a_delta);  // Elden Ring moved its world origin

	private:
		MantlePlan    plan_;
		Phase         phase_{ Phase::kIdle };
		std::uint64_t askedMs_{ 0 };
		std::uint64_t startMs_{ 0 };
		bool          guest_{ false };
	};
}
