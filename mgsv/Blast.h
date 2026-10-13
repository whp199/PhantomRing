#pragma once
// Snake's grenades and explosives as kGEvExplosion (docs/GUEST.md 18.5).
//
// MGSV makes every blast through one place: its attack-area system's vf0 (Addresses.h kAttackArea), with the centre,
// the radius and the attack id (TppDamage.ATK_*). Thrown grenades, gas clouds (one call per "puff" while the cloud
// lasts), molotovs, C4, claymores and mines all arrive here. The attack id decides (logic::KindOfAttack):
// lethal / sleep / stun / smoke go to Elden Ring; blasts of gun-fired rounds (rockets, grenade launchers) don't --
// those rounds already went out as kGEvFire with the explosive bit and blast where they hit in Elden Ring's world.

#include "Logic.h"

#include <cstdint>
#include <string>

namespace phantom::guest::blast
{
	// DllMain: the detour, behind the byte checks (refused = no explosion events, logged).
	bool Install();

	// Game thread, every player-camera frame: whether the sandbox is ready (blasts before that are boot noise), the
	// frame number for the events, and Snake's root (blasts far from him are MGSV's world, not his).
	void Observe(bool a_ready, std::uint64_t a_frame, const float a_root[3]);

	// A new throw/placed id: one counter for both, so a kGEvExplosion's pad[1] names exactly one of them.
	std::uint32_t NextId();

	// proto::AmmoKind of a blast kind (kNone = smoke: no damage).
	std::uint32_t ProtoKind(logic::BlastKind a_kind);

	std::string Describe();
}
