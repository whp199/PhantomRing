#pragma once

// Snake's explosive rounds in flight (rockets, missiles, grenade-launcher rounds). MGSV flies them over its own airport and
// never shows Elden Ring anything; the host used to put the blast at Elden Ring's impact point the instant the shot left
// the gun, often 30-70 m away, so the player never saw it go off (session 4). Now each round flies from Snake's muzzle at
// its speed ([Combat] round_speed_rocket / round_speed_grenade), drawn as a small dark body with a bright exhaust trail
// (er/RenderMarkers.h), and goes off where it meets Elden Ring's world or a hostile character on the way (one segment per
// frame, re-traced: an enemy that steps into its path takes it) -- or at the end of its range. The blast is the old one:
// Elden Ring's fireball, the damage of Combat::BlastAt, and the object-only attack that breaks crates and carts.

#include "Host.h"
#include "Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er::Rounds
{
	// MGSV TppEquip bullet id -> m/s of its flight, 0 = no flight (goes off at once). Pure (tests/test_host_combat.cpp).
	enum class Flight : std::uint8_t
	{
		kNone,
		kRocket,   // BL_ms* missiles / rockets, BL_20mmRocket, the vehicle rockets, BL_HAND_KillRocket
		kGrenade,  // grenade pistol / launcher rounds, mortar
	};
	Flight FlightOf(std::uint32_t a_bulletId);
	// m/s of a round of a_kind a_age s after the shot: a rocket leaves at a_launch and its motor takes it to a_boost after a_after s
	// (over 0.3 s); other rounds keep a_launch. Pure (tests).
	float SpeedAt(Flight a_kind, float a_age, float a_launch, float a_boost, float a_after);

	// One explosive round from a_muzzle along a_dir (Elden Ring world). False: not flown (no speed for it, flight off):
	// the caller blasts at once as before.
	bool Launch(const Frame& a_frame, std::uint32_t a_bulletId, const Vec3& a_muzzle, const Vec3& a_dir, float a_radius, std::string a_what);

	void Tick(const Frame& a_frame);  // FrameBegin, after the events: fly, hit, blast; markers
	void OnPuppetStop();
	void OnWorldShift(const Vec3& a_delta);
	std::string Describe();
}
