#pragma once

// The Tarnished as an invisible puppet driven by Snake (DESIGN.md §5.1; SkyCraft's PlayerPuppet).
//
// Starts when the guest is ready and a character is in the world; stops (and restores everything it
// changed: visibility, NoDead/NoDamage) the same frame the guest disappears, a load starts, the
// player dies, or the player object changes. While active:
//  - yaw = Snake's MGSV yaw + delta (delta fixed at start: the puppet keeps the facing it had);
//  - motion = odometer delta rotated by delta, resolved by the kinematic controller against ER's
//    world, applied with ChrIns::SetPosition (game warp path); or, with motion = root_motion, handed to
//    ER's own character proxy as root motion (er/RootMotion.h); single-frame jumps are clamped;
//  - HP: NoDead, HP reset every frame (mirroring Snake's life fraction); any loss becomes kHEvHurt
//    (with hit_hook, hits are caught and cancelled in ReceiveDamage instead: er/HitHook.h), scaled so Snake loses
//    the share of his life the Tarnished would have lost ([Combat] hurt_share);
//  - an Elden Ring interaction (er/Interaction.h) or cutscene: Snake holds still.

#include "Anchor.h"
#include "Host.h"
#include "Kinematic.h"

#include <string>

namespace phantom::er
{
	namespace Puppet
	{
		void Decide(const Frame& a_frame);         // FrameBegin
		void Move(const Frame& a_frame);           // ChrIns_PrePhysics
		void AfterPhysics(const Frame& a_frame);   // WorldChrMan_PostPhysics
		void SyncAnchor(const Frame& a_frame);     // anchor feet = the puppet's position after ER's physics

		void OnGuestDied(const Frame& a_frame);    // kGEvDied
		void OnWorldShift(const Vec3& a_delta);    // er/WorldShift.h: the puppet's own stored positions move with the world

		[[nodiscard]] bool          Active();
		[[nodiscard]] const Anchor& CurrentAnchor();  // valid while Active()
		[[nodiscard]] bool          Riding();
		[[nodiscard]] float         GroundY();
		[[nodiscard]] float         FallSpeed();
		[[nodiscard]] float         HpFrac();
		// Seconds since Snake (or the puppet) died, until Elden Ring respawns the player; -1 when nobody is
		// dying. While >= 0 the renderer fades Snake out ([Render] death_visual) and the Tarnished stays hidden.
		[[nodiscard]] float         DeathSeconds();
		// From a death (Snake's, or Elden Ring's own) until Elden Ring respawns the player. Not the dead bit: Elden Ring
		// clears that ~6 s after YOU DIED while the screen is still fading, seconds before the respawn load (VERIFIED
		// live). A respawn is a living player after a load (or a different player object), or 15 s alive without one.
		[[nodiscard]] bool          Dying();
		bool                        Usable(std::string* a_why = nullptr);  // addresses verified for puppeting

		enum class Override
		{
			kAuto,
			kOff
		};
		void        SetOverride(Override a_mode);
		// Console `puppet motion kinematic|root_motion`: switches [Puppet] motion live (the root-motion hook installed or
		// removed on the running puppet).
		std::string SetMotion(std::string_view a_mode, const Frame& a_frame);
		std::string Describe();
	}
}
