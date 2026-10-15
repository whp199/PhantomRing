#pragma once
// Snake's placed explosives (claymores, AP/sleep/stun mines, C4) as kGEvPlaced / kGEvPlacedGone, and kHEvDetonate
// (docs/GUEST.md 18.8).
//
// MGSV's proximity mines only trigger on MGSV's own characters, and there are none in the sandbox: Elden Ring's enemies
// have to set them off. MGSV mirrors every object Snake places into vars (placedWeapons/Pos/Rot, saved with the game),
// so the guest polls that each frame and tells the host where each one is; when an Elden Ring enemy walks into one's
// trigger radius the host sends kHEvDetonate and the guest sets it off through MGSV's own path: the "set off" bit its
// Placed System tests every frame (Addresses.h). Its blast then reaches the attack-area system like any other and
// goes out as kGEvExplosion with the placed id (Blast.cpp). The EMN mine casts a net instead of a blast: that one's
// explosion event is the guest's own. C4 is the player's: MGSV's detonator (or the guest's, C4.h) sets the same bit.
// Decoys (inflatable / active / shock, [Combat] Decoys) go out the same way with a = kAmmoDecoy (roadmap 2.8): Elden Ring's
// enemies stare at them (er/Decoys.h); a kHEvDetonate for one means it popped there: kGEvPlacedGone, never announced again.

#include "Logic.h"

#include <cstdint>
#include <string>

namespace phantom::guest::placed
{
	// DllMain: byte checks (refused = no placed events / no detonation, logged).
	void Init();

	// Game thread, every player-camera frame: new/gone placed explosives -> events; a_root = Snake (far ones are MGSV's world).
	void Update(bool a_ready, std::uint64_t a_frame, const logic::Vec3& a_root);

	// kHEvDetonate (game thread): set that one off in MGSV.
	void Detonate(std::uint32_t a_id);

	// Console (`!detonate <id>`, any thread): the same as a kHEvDetonate, on the next Update.
	void RequestDetonate(std::uint32_t a_id);

	// Blast.cpp: the placed explosive a blast at a_centre came from -- its id, 0 = none of ours; a_setOff = whether
	// kHEvDetonate set it off (else MGSV's own world did: D-Walker walking into a claymore, a grenade next to a mine).
	std::uint32_t MatchBlast(const logic::Vec3& a_centre, logic::BlastKind a_kind, bool* a_setOff = nullptr);

	// C4's detonator (C4.cpp, docs/GUEST.md 18.8a). Sets off Snake's oldest live C4 (of a_inHand's kind first, c4::PickOldest)
	// through the same bit as kHEvDetonate -- every live one with a_all; how many went. Any thread.
	std::uint32_t SetOffC4(std::uint16_t a_inHand, bool a_all, const char* a_why);
	[[nodiscard]] bool HaveLiveC4();
	// Running count of Snake's C4 that MGSV's own SetOff set off (its detonator, normally).
	[[nodiscard]] std::uint64_t MgsvC4SetOffs();

	std::string Describe();
}
