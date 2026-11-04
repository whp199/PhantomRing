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

#include <optional>
#include <string>
#include <vector>

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

		// Elden Ring's world for what Snake throws or sets down (er/Throw.cpp's arcs, er/Decoys.cpp's set-down; review 2026-10-31):
		// ray_filter (the world, enemies' hit capsules) and his body's ground filter (the floors and walls only characters meet:
		// Stormveil's scaffold, whose planks ray_filter passes through), the nearer hit. a_groundOnly: the ground filter alone (a
		// decoy stands on the floor, never on an enemy's hit capsule). Without the collision layer table: ray_filter alone.
		bool CastWorld(const Vec3& a_from, const Vec3& a_delta, Vec3& a_hit, const std::uint8_t* a_ignore, bool a_groundOnly = false);

		[[nodiscard]] bool          Active();
		[[nodiscard]] const Anchor& CurrentAnchor();  // valid while Active()
		// (0.8.0 polish) Bumped whenever the anchor is made anew (the puppet starts, or Elden Ring put the player somewhere else): what
		// is mapped through it may jump then (D-Dog's step guard starts over, er/BuddyGuard.h).
		[[nodiscard]] std::uint32_t AnchorEpoch();
		// Bumped only when the puppet starts (Start). A re-anchor of 1.5-5 m (Elden Ring shoving the player, a grab, the climb-up's
		// abort) is not a new start: D-Dog's guards keep going through it, so the mapped dog's jump with the anchor is held / eased
		// like any other (review 0.8.0: they started over and drew it at once); over 5 m they start over by distance.
		[[nodiscard]] std::uint32_t StartEpoch();
		// While MGSV plays a staged move (kGuestClimbing / kGuestMantling: Snake at one of the airport's ladders or obstacles) and the
		// puppet runs: the anchor his draw list is drawn with, turned so that his body there faces Elden Ring's move (er/StagedFacing.h)
		// -- a_listYaw: the list's anchorYaw. nullopt: CurrentAnchor() as it is. Only for his drawn body: the camera, the buddy and
		// everything mapped from around his home keep CurrentAnchor().
		[[nodiscard]] std::optional<Anchor> StagedDrawAnchor(float a_listYaw);
		[[nodiscard]] bool          Riding();
		[[nodiscard]] bool          RidingHorse();  // ...D-Horse (its own speed, step-up, radius and eye height)
		[[nodiscard]] float         GroundY();
		[[nodiscard]] float         FallSpeed();
		[[nodiscard]] bool          Jumping();  // in the air from a jump of Snake's (kGuestJumping, er/Kinematic.h KinematicJump)
		[[nodiscard]] float         HpFrac();
		// Seconds since Snake (or the puppet) died, until Elden Ring respawns the player; -1 when nobody is
		// dying. While >= 0 the renderer fades Snake out ([Render] death_visual) and the Tarnished stays hidden.
		// Only for Snake's own deaths: a death of the Tarnished while the player played him (Tarnished mode, Snake not playing)
		// is -1 here -- he dies on screen as he is, and Snake isn't drawn at the anchor Tarnished mode left him at (live
		// 2026-10-31: Snake popped up over the dying Tarnished). Dying() covers both (the respawn rule).
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
		void        SetOverride(Override a_mode);  // console `puppet off|auto` (auto also ends a step-aside)
		// What else keeps the puppet off while it would run: the player took the Tarnished (Tarnished mode, er/Host.cpp), Elden
		// Ring walks him through the mist ([Input] release_prompts), or he climbs a ladder (er/Ladder.h). Each is its own reason;
		// the puppet runs when none holds.
		enum class Hold : std::uint8_t
		{
			kConsole = 1,
			kTarnished = 2,
			kStepAside = 4,
			kLadder = 8,
		};
		void               SetHold(Hold a_reason, bool a_on);
		[[nodiscard]] bool Held(Hold a_reason);
		// Ladders (er/Ladder.h, [Ladder] snake): while Elden Ring climbs one with the Tarnished (Hold::kLadder: the puppet is off,
		// Elden Ring has the pad and its camera), Snake is drawn on it instead. a_on (FrameBegin, before Decide, every frame): the
		// Tarnished stays hidden through the puppet's stop and after it, and the renderer draws Snake at the anchor SetClimbAnchor
		// gives (his feet where the Tarnished's are, facing the ladder); false shows the Tarnished again (unless the puppet runs).
		void               SetClimbVisual(bool a_on);
		void               SetClimbAnchor(const Anchor& a_anchor);  // after Elden Ring's physics, every frame of the climb
		[[nodiscard]] bool ClimbVisual();  // Snake is drawn on a ladder (the puppet off): RenderScene, Hud
		// Console `puppet motion kinematic|root_motion`: switches [Puppet] motion live (the root-motion hook installed or
		// removed on the running puppet).
		std::string SetMotion(std::string_view a_mode, const Frame& a_frame);
		// Console `mantle [plan [yaw_deg] | go [yaw_deg] | on|off | run off|low|all | hints reset]`: what a climb up / vault would do in
		// front of Snake (plan), one started as Y would (go), [Mantle] switched live.
		std::string MantleCommand(const std::vector<std::string>& a_words, const Frame& a_frame);
		std::string Describe();
	}
}
