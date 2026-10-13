#pragma once
// When does an Elden Ring death count as over (er/Puppet.cpp AfterPhysics -> kHEvRespawn, puppet back)?
// ER clears the dead bit ~6 s after YOU DIED, before its respawn load (VERIFIED live), and near a Stake of
// Marika the player then sits in "Choose revival location" with the dead bit clear, no blocking menu and
// only ER's hidden HUD to show it (VERIFIED live). Declaring the respawn there starts the puppet, which
// masks A and the stick: the player could never choose. So:
//  - a load (or another player object) since the death always counts;
//  - with the HUD signal known (seen visible since the last load), 15 s alive with the HUD up counts (a
//    revival without a load);
//  - with ER's HUD off (or the signal disabled) the 15 s would run inside the dialog: only a revival that
//    moved the player away from where it died counts, or a long cap so a revival we can't see never
//    leaves Snake down for good (UNVERIFIED live: ER's HUD off was never tested).

#include <cstdint>

namespace phantom::er
{
	struct RevivalInput
	{
		bool          load{ false };      // a load or another player object came since the death
		bool          alive{ false };     // alive again (and, with the HUD known, HUD up) ...
		std::uint64_t aliveMs{ 0 };       // ... for this long
		bool          hudKnown{ false };  // Frame::hudKnown
		float         movedM{ 0.0f };     // distance from where the player died
	};

	inline constexpr std::uint64_t kReviveQuietMs = 15000;   // HUD known: alive with the HUD up this long
	inline constexpr std::uint64_t kReviveCapMs = 120000;    // HUD unknown: give up waiting after this
	inline constexpr float         kReviveMovedM = 5.0f;     // HUD unknown: a revival warped the player this far

	inline bool Revived(const RevivalInput& a_in)
	{
		if (a_in.load) {
			return a_in.alive;
		}
		if (!a_in.alive) {
			return false;
		}
		if (a_in.hudKnown) {
			return a_in.aliveMs > kReviveQuietMs;
		}
		return a_in.movedM > kReviveMovedM || a_in.aliveMs > kReviveCapMs;
	}
}
