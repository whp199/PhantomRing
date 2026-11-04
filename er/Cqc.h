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
#include <string_view>
#include <vector>

namespace phantom::er::cqc
{
	struct Rules;
}

namespace phantom::er::Cqc
{
	cqc::Rules CurrentRules();  // [Cqc] as every CQC hit uses it (D-Dog's flinch and knockdown are CQC's: er/DogLogic.h)
	void OnEvent(const Frame& a_frame, const proto::GuestEvent& a_event);
	void Tick(const Frame& a_frame);  // FrameBegin, after the events: ends holds
	void OnPuppetStop();               // lets every held enemy go
	bool IsHeld(std::uint64_t a_handle);  // CQC holds him now (his ChrIns+0x538 hold bits are ours: Stealth leaves him be)
	// D-Dog's "Keep 'em busy" (er/Dog.h, docs/research/ddog.md §3.6): the bitten one held a_seconds by the same bookkeeping as
	// Snake's grab, so a dog hold and a CQC hold never stack on one enemy: one owner of his bits, put back once. False (nothing
	// held: the bite is a flinch only) when Snake holds him, a Fulton balloon has him, he was let go less than hold_cooldown ago,
	// he is a boss (CQC's rule), or he isn't in Elden Ring's list. No leave_distance rule (the dog has him, not Snake). Snake's grab
	// of the one the dog holds takes the hold over (choke, interrogate, throw: he is Snake's then); a new dog hold lets the last go.
	bool HoldFor(std::uint64_t a_handle, float a_seconds, std::string_view a_who);
	void LetGo(std::uint64_t a_handle, const char* a_why);  // ends D-Dog's hold on him, if it has one
	bool DogHolds(std::uint64_t a_handle);
	// A boss by CQC's HP rule alone (cqc::IsBoss: [Cqc] boss_hp / boss_bigness against the Tarnished's HP; an unknown max HP counts
	// as one).
	bool IsBossHp(std::int32_t a_maxHp);
	// A boss: the HP rule, or Elden Ring's own mark of one -- his NpcParam row's isSoulGetByBoss bit (params::kNpcBossBits); an
	// NpcParam that can't be read counts anything bigger than an ordinary enemy (cqc::IsBoss with a_npcBoss). Every rule that must
	// leave a boss alone asks this: CQC's grab, D-Dog's pick / bite / bark / hold, decoys, the box poster, the Fulton, the ActorTable's
	// kActorBoss (review 0.8.0: the HP rule alone let a ~1000 HP dungeon boss be held, slept or killed).
	bool IsBoss(std::int32_t a_npcParam, std::int32_t a_maxHp);

	// Dev console: `cqc [status | grab | throw | choke | interrogate | release | on | off]` -- an action as if the guest had sent it
	// (Snake's place and facing from the puppet), for testing without MGSV's controls.
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
	std::string Describe();
}
