#pragma once
// Snake's thrown explosives as kGEvThrow, and the arc he is aiming (docs/GUEST.md 18.8).
//
// MGSV simulates a grenade over its own flat airport, which isn't Elden Ring's ground; the host flies its own copy
// through Elden Ring's collision. So it needs what MGSV's throw starts with: the release point and the initial
// velocity. Both come from MGSV's Throwing System at the moment it creates the projectile (Spawn, Addresses.h), and
// the arc MGSV draws while Snake aims (its "throwing line") starts with exactly the same numbers, so the host can draw
// the preview from the GuestState too. The blast that follows carries the throw id in its kGEvExplosion (Blast.cpp).

#include "Logic.h"

#include <cstdint>
#include <string>

namespace phantom::guest::throwing
{
	// DllMain: the Spawn detour and the arc reader, each behind its byte checks (refused = no kGEvThrow / no aim, logged).
	bool Install();

	// The arc Snake aims right now, for GuestState (kGuestThrowAim).
	struct Aim
	{
		bool          valid{ false };
		float         pos[3]{};
		float         vel[3]{};
		std::uint32_t kind{ 0 };  // proto::AmmoKind
	};

	// Game thread, every player-camera frame: retires throws whose projectile is gone and reads the aim arc.
	// a_aiming = PlayerVars HOLD, a_inHand = vars.currentInventorySlot, a_support = the selected support item's equip id.
	Aim Observe(bool a_ready, std::uint64_t a_frame, bool a_aiming, std::uint32_t a_inHand, std::uint16_t a_support);

	// Blast.cpp (inside MGSV's update): the throw a blast at a_centre came from -- its id, 0 = none of ours.
	std::uint32_t MatchBlast(const logic::Vec3& a_centre, logic::BlastKind a_kind);

	std::string Describe();
}
