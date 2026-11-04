#pragma once

// Snake's shots make noise in Elden Ring (docs/HOST.md "Noise"; [Noise]; protocol v16 kFireSuppressed / kGuestSuppressorOn).
//
// Elden Ring's enemies hear "AI sounds": every attack the Tarnished makes emits one (AtkParam appear / hit AI sound ids:
// AiSoundParam 2100, 4 m, as a swing appears; 2010, 12 m, where a weapon hits), so do his footsteps, pots, breaking crates
// and the animation events of enemies (TAE 237). Each is a request to CSWorldAiManager's sound system (+0x7628: a list of
// 0x90-byte sounds with a position, an owner, an AiSoundParam row and a radius = the row's x the request's scale); every
// AI's listener checks the sounds near it each update (0x14033F940 -> 0x140349C00: within the radius plus its head size, of
// a hostile owner, the priority) and takes the loudest as its sound target -- the "huh?" turn, the walk over to look, the
// search -- through its own sound behaviour (NpcThinkParam sound_behavior_id; Limgrave's soldiers 300000 / 431000).
//
// MGSV's shots are never Elden Ring attacks, so they made none: an unsuppressed rifle next to a camp alerted nobody. Now
// each unsuppressed shot of Snake's (and the buddy's) makes one at the muzzle, owned by the hidden Tarnished (who stands
// where Snake does), radius by weapon class ([Noise] pistol < rifle < sniper < shotgun < launcher); a round that strikes the
// world makes a small one where it lands ([Noise] impact). A shot through a working suppressor (kFireSuppressed) makes none
// ([Noise] suppressed = 0), shows no muzzle flash, and its hit bullet carries no AI sound either (er/Combat.cpp: the
// Throwing Dagger row's own 2100 / 2000, 4 m round the one hit, are written -1 for it), so a dart puts one soldier of a
// group to sleep without the others hearing it.
//
// [Noise] mode = heard is the fallback if Elden Ring's sound can't be emitted (an address not verified, the row missing):
// enemies within the radius that aren't fighting already are told by a token hit (no damage, no flinch) -- Elden Ring's
// aggro on the Tarnished, not a search. mode = off: no noise at all.
//
// "Suppressor worn out" (corner, warn): kGuestSuppressorOn going off right after a suppressed shot, the same weapon in hand
// (noise::SuppressorWatch).

#include "Host.h"
#include "Math.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::er::Noise
{
	// A shot left Snake's (a_buddy: the buddy's) weapon at a_muzzle; a_impact is where it struck the world (a_struckWorld) --
	// called by Combat::OnShot for every kGEvFire / kGEvBuddyFire, after the round's own hit; a_target is the handle of the character
	// it hit (0: none; mode heard leaves him out: his hit is Elden Ring's aggro already).
	void OnShot(const Frame& a_frame, const Vec3& a_muzzle, const Vec3& a_impact, bool a_struckWorld, std::uint32_t a_bulletId, bool a_explosive, bool a_flame,
		bool a_suppressed, bool a_buddy, std::uint64_t a_target, std::string_view a_what);
	// Elden Ring's own AI sound at a_pos, a_radius m, owned by the Tarnished. False when it can't be made (logged once).
	bool Emit(const Frame& a_frame, const Vec3& a_pos, float a_radius, std::string_view a_what);
	void Tick(const Frame& a_frame);  // FrameBegin, after the events: the suppressor watch

	std::string Describe();
	// Dev console: `noise` | `noise emit [r] [m ahead]` (an AI sound in front of the Tarnished) | `noise mode sound|heard|off` |
	// `noise set <pistol|rifle|sniper|shotgun|launcher|flame|suppressed|impact|row> <v>` | `noise who [r]` (the AI state of
	// the characters around: in battle, searching, the last AI sound each heard).
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
}
