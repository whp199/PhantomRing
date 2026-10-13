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

#include <cstdint>
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
		void        Draw();
		const char* TypeName(std::uint16_t a_type);
		[[nodiscard]] bool  HasEstimate();
		[[nodiscard]] Vec3  Position();      // ER space, ground-snapped
		[[nodiscard]] float GroundOffset();  // add to the mapped buddy draws' Y (m)
		std::string Describe();
	}
}
