#pragma once

// Snake's thrown explosives in Elden Ring (protocol v5/v6; pure rules in er/ThrowLogic.h).
//
// MGSV flies its grenade over its own flat airport apron, which is not Elden Ring's ground: a throw that should
// bounce off an Elden Ring wall or roll down its hill lands somewhere else in MGSV, and the blast MGSV reports came
// through the anchor from there (and from wherever MGSV's treadmill had put Snake since). So the host flies its own
// copy:
//  - kGEvThrow (release point + velocity, MGSV world): mapped through the anchor and flown through Elden Ring's
//    collision (cs_phys_world_cast_ray) with MGSV's gravity, bouncing and rolling; drawn in flight and where it lies
//    (er/RenderMarkers.h, a small lit ball).
//  - the matching kGEvExplosion (pad[1] = the throw id) puts the blast -- damage and Elden Ring's pot effect --
//    where OUR copy is. Impact explosives (fuse 0: sleep, stun, smoke grenades) go off where our copy first
//    touches Elden Ring's world (with the radius MGSV reported for that equip id before, else [Combat]
//    throw_radius_*), MGSV's own report then only refreshes a cloud; fused ones (hand grenades) go off when MGSV
//    says, where our copy lies by then. A gas or smoke cloud's later reports stay on our copy.
//  - while GuestState kGuestThrowAim is set (v6: aimThrowPos/Vel/Kind), MGSV's aiming arc is drawn through Elden
//    Ring's collision: a thin line of bright dots and a ring where it lands, depth-tested, not in menus/cutscenes.

#include "Host.h"
#include "Math.h"

#include <optional>
#include <string>
#include <vector>

namespace phantom::er::Throw
{
	void OnThrow(const Frame& a_frame, const proto::GuestEvent& a_event);
	// A kGEvExplosion that belongs to one of our throws (pad[1]): handled here (blast at our copy). False: not ours.
	bool OnExplosion(const Frame& a_frame, const proto::GuestEvent& a_event);
	void Tick(const Frame& a_frame);   // FrameBegin, after the events: fly, land, impact blasts, the arc, markers
	void OnPuppetStop();               // MGSV's grenades are no longer ours to follow
	void OnWorldShift(const Vec3& a_delta);  // Elden Ring moved its world origin: every stored position moves with it

	std::string Describe();
	// Dev console: `throw [status] | throw arc on|off|auto`.
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
}
