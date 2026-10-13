#pragma once

// Elden Ring hits on the puppet, caught where the game applies them (re-backlog E2, [Puppet] hit_hook):
// CSPlayerDamageModule vfunc 7 ReceiveDamage(dm, attacker, ChrDamage*) at 0x44D050, taken on the
// player's damage module only (VptrSwap; nothing patched). While the puppet is active ER applies the
// hit as usual, the HP it took (ER's own attack/defence math; the amount is not in ChrDamage at entry,
// VERIFIED live) is measured and given straight back, and the hit (amount, where it came from) goes to
// MGSV, whose life is authoritative. Damage over time, poison and fall damage don't
// pass here; the HP-delta check in Puppet still covers them.
//
// Statically verified (decompilation of the hit path); UNVERIFIED live: one call per landed hit, the
// meaning of ChrDamage fields beyond amount/position/direction, and the hook's thread (hits are queued
// here and turned into kHEvHurt by the puppet's task).

#include "Game.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	namespace HitHook
	{
		struct Hit
		{
			std::int32_t  amount{ 0 };        // ChrDamage+0x228: total HP damage
			Vec3          from;               // unit, Havok space: towards where the hit came from
			Vec3          at;                 // ChrDamage+0x1A0: hit position (Havok)
			std::uint8_t* attacker{ nullptr };
			std::uint32_t kind{ 0 };          // ChrDamage+0x22C (UNVERIFIED meaning: damage type?)
			std::uint32_t atkParam{ 0 };      // ChrDamage+0x234 (UNVERIFIED: AtkParam id?)
		};

		bool Install(const game::Player& a_player);  // swaps the player's damage module vptr (verified first)
		void Remove(const game::Player& a_player);
		void SetCancel(bool a_cancel);               // skip the original (puppet active and not dying on purpose)
		std::vector<Hit> Drain();
		[[nodiscard]] bool Installed();
		std::string Describe();

		// Live RE: while studying, hits pass through (never cancelled) and the first few are recorded raw with
		// the HP lost, so the ChrDamage field holding the amount can be identified ("hithook dump").
		void        SetStudy(bool a_on);
		std::string DumpStudy();
	}
}
