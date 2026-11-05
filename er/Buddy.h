#pragma once

// Battle buddies, D-Walker first (DESIGN.md §10). MGSV runs the buddy (deploy, follow, commands,
// riding, his gun); the host only places what crosses over:
//  - riding: Snake on D-Walker = kGuestOnVehicle; the odometer follows the walker, so the puppet
//    moves at D-Walker's speeds ([Puppet] max_speed_vehicle / step_up_vehicle) and the camera
//    collision uses the mounted eye height;
//  - position: GuestState's buddy pose (protocol v2: buddyPos/buddyYaw with kBuddyPosValid), or as a
//    fallback the centroid of the guest's kOwnerBuddy draws (phase 2 capture); mapped through the
//    anchor, then snapped to ER ground with a ray (he walks on MGSV's flat sandbox). GroundOffset()
//    is the vertical correction the buddy renderer applies (phase 4);
//  - presence: kBuddyPresent / kBuddyMissing (not developed in the save) / kBuddyRidden are logged;
//  - his gun: kGEvBuddyFire is traced like Snake's shots (er/Events.cpp); damage is phase 3/4.

#include "Math.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>

namespace phantom::er
{
	struct Frame;

	namespace Buddy
	{
		enum Type : std::uint16_t  // MGSV BuddyType (vars.buddyType); order UNVERIFIED beyond DOG = 2 (MGS5VR tests)
		{
			kNone = 0,
			kHorse = 1,
			kDog = 2,
			kQuiet = 3,
			kWalkerGear = 4,
		};

		void        Update(const Frame& a_frame);  // WorldChrMan_PostPhysics, after the draw list was acquired
		// MGSV has no buddy out (2026-10-14, the player: "buddy changing and dismissing"): its buddy type is NONE, or the buddy
		// it had out is gone (kBuddyPresent cleared, not loading). Its draws are left out and it is not placed.
		[[nodiscard]] bool Absent();
		// Pure (tests): the corner message for a buddy change seen in GuestState, or "" for none. a_before/a_after: buddy type
		// and whether one is out (Present or Loading).
		std::string ChangeText(std::uint16_t a_typeBefore, bool a_outBefore, std::uint16_t a_typeAfter, bool a_outAfter);
		void        Draw();
		const char* TypeName(std::uint16_t a_type);
		[[nodiscard]] bool  HasEstimate();
		[[nodiscard]] Vec3  Position();      // ER space, ground-snapped (and moved by DrawShift)
		[[nodiscard]] float GroundOffset();  // add to the mapped buddy draws' Y (m)
		// (D-Dog round, docs/research/ddog.md §3.7) Where Elden Ring has no ground within 8 m under the mapped buddy (beside a tower
		// top, a cliff's edge after a ladder) it no longer floats at MGSV's height: the floor at Snake's height (+-1.5 m) under it,
		// else the nearest such floor on the line toward Snake -- this horizontal ER-space shift moves its drawn body there (render
		// only: MGSV's buddy is untouched). 0 otherwise, and while ridden.
		[[nodiscard]] Vec3  DrawShift();
		// (0.8.0 polish) A waiting D-Dog's Elden Ring correction (er/BuddyGuard.h WaitPin): already in Position(); the renderer adds it to
		// the dog's draws unsmoothed (it changes as Snake slips at a wall, a few cm a frame). 0 for other buddies and once eased out.
		[[nodiscard]] Vec3  WaitShift();
		// (v18) Quiet off her post (MGSV's place) has no Elden Ring floor under her within [Quiet] far_ground_up / far_ground_down: she is
		// not drawn (never floating, never buried).
		[[nodiscard]] bool  QuietNoFloor();
		// kGEvBuddyOrder (er/Events.cpp): D-Dog's Wait (9) starts the pin, its whistle (8) / DD (16) end it.
		void OnOrder(std::uint32_t a_command, std::uint32_t a_buddyType);
		// kGEvDogAttack (er/Events.cpp): an attack order MGSV took (kDogOrdered) ends the pin too -- the dog runs from its Wait (review
		// 0.8.0); a refused one doesn't (the dog still waits). Nothing with [Dog] enabled = false.
		void OnDogAttack(std::uint32_t a_phase);
		// The ground under the unguarded place (D-Dog: where RenderScene's own guard may draw the body before the placement takes the
		// move); equal to GroundOffset() / DrawShift() whenever the placement guard isn't holding anything.
		[[nodiscard]] float DrawGroundOffset();
		[[nodiscard]] Vec3  DrawGroundShift();
		void OnWorldShift(const Vec3& a_delta);  // er/WorldShift.h: D-Dog's kept places move with Elden Ring's origin
		// Pure (tests): the floor rule. a_floorAt(p) = Elden Ring's floor near a_snakeY under p (nullopt: none); returns the
		// shift (horizontal) and the floor's height, or nullopt when there is none even next to Snake.
		struct Floor
		{
			Vec3  shift;
			float y{ 0 };
		};
		// Never searched for a buddy more than kFloorSearchMax m from Snake (review 0.8.0: a horse left 150 m away cost ~300 rays a
		// frame, and the renderer drops a shift over 30 m anyway): nullopt, no ray at all.
		inline constexpr float kFloorSearchMax = 30.0f;
		template <class F>
		std::optional<Floor> FloorNearSnake(const Vec3& a_buddy, const Vec3& a_snake, F&& a_floorAt, float a_step = 0.5f)
		{
			const Vec3  d{ a_snake.x - a_buddy.x, 0, a_snake.z - a_buddy.z };
			const float len = LengthXZ(d);
			if (!(len <= kFloorSearchMax)) {
				return std::nullopt;
			}
			if (const auto y = a_floorAt(a_buddy)) {
				return Floor{ {}, *y };
			}
			if (!(len > 1e-3f) || !(a_step > 0.01f)) {
				return std::nullopt;
			}
			for (float t = a_step; t <= len + 1e-3f; t += a_step) {
				const Vec3 shift = d * (std::min(t, len) / len);
				if (const auto y = a_floorAt(a_buddy + shift)) {
					return Floor{ shift, *y };
				}
			}
			return std::nullopt;
		}
		std::string Describe();
	}
}
