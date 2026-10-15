#pragma once

// CQC holds and throws on Elden Ring enemies (ROADMAP 2.2 tier 1, protocol v13 kGEvCqc, docs/COMBAT.md §7a). MGSV grabs only its
// own soldiers; the guest sends the player's CQC intent (mgsv/Cqc.h) and this host answers it on the Elden Ring enemy in reach in
// front of Snake (er/CqcLogic.h picks him and the answer):
//  - grab: a small flinch and he is held in place (ChrIns+0x538 no-attack / no-move, the bits Stealth's poster uses) until the
//    release, a throw, hold_seconds, or Snake walking leave_distance away -- every bit we set goes back, nothing else (also on
//    his death; a Fulton lift takes the bits over); then no new hold on him for hold_cooldown (a re-grab is a flinch);
//  - throw: let go, then a knockdown (throw_poise and throw_dmg_level, hit_correction and a token 1 HP);
//  - choke: [Combat] choke_sleep / choke_share of sleep build-up (the sleep ladder): enough to put an ordinary enemy to sleep;
//  - interrogate: a corner line (Hud::Notify), nothing else; choke and interrogate only on the one held;
//  - a boss (combat::Bigness >= boss_bigness, max HP >= boss_hp, or not known) is never held, thrown or choked: a stagger.
// Every hit waits for Combat's bullet row (MGSV's own punch of the same press holds it ~50-300 ms): settle_ms, then retry_ms.
// [Cqc] enabled = false by default (UNVERIFIED live): the event is then only logged.

#include "Host.h"

#include "phantomring_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er::Cqc
{
	void OnEvent(const Frame& a_frame, const proto::GuestEvent& a_event);
	void Tick(const Frame& a_frame);  // FrameBegin, after the events: ends holds
	void OnPuppetStop();               // lets every held enemy go
	bool IsHeld(std::uint64_t a_handle);  // CQC holds him now (his ChrIns+0x538 hold bits are ours: Stealth leaves him be)

	// Dev console: `cqc [status | grab | throw | choke | interrogate | release | on | off]` -- an action as if the guest had sent it
	// (Snake's place and facing from the puppet), for testing without MGSV's controls.
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
	std::string Describe();
}
