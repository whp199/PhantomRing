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

	// The puppet stops: does the Tarnished stay hidden (er/Puppet.cpp Stop)? While a death is under way -- the stop's reason is
	// the dead bit, or Snake's death already killed him (KillForGuest), or the puppet's character is the one dying -- he stays
	// hidden until the respawn and Snake keeps being drawn where he died (HOST.md "Death visuals"), whatever else stopped the
	// puppet that frame (the chord, a step-aside, MGSV's heartbeat, a ladder). Before 0.6.0 only the reason "player dead" kept
	// him hidden: a stop for anything else in the frames between the kill and Elden Ring's dead bit showed him dying on screen,
	// with nothing of Snake's drawn (the puppet off, no death recorded). A ladder (Snake drawn on it) keeps him hidden too.
	enum class StopVisual : std::uint8_t
	{
		kShow,            // Elden Ring's Tarnished again (Tarnished mode, the mist, Snake not playing)
		kHiddenForDeath,  // until the respawn
		kHiddenForClimb,  // while Elden Ring climbs a ladder with him and Snake is drawn on it
	};
	inline StopVisual AtStop(bool a_reasonDead, bool a_deathUnderWay, bool a_killedForSnake, bool a_ladderVisual)
	{
		if (a_reasonDead || a_deathUnderWay || a_killedForSnake) {
			return StopVisual::kHiddenForDeath;
		}
		return a_ladderVisual ? StopVisual::kHiddenForClimb : StopVisual::kShow;
	}

	// Why the running puppet stops this frame (er/Puppet.cpp Decide); nullptr = it runs on. The puppet's own character dead in a
	// world that is still loaded comes first: before 0.6.0 (review) "guest lost", Tarnished mode, a step-aside, a ladder or
	// "disabled" were tested before the dead bit, so a stop in a death's first frames carried another reason (and only "player
	// dead" kept the Tarnished hidden: AtStop now looks at the death too, this keeps the log honest).
	struct StopInput
	{
		bool samePlayer{ true };  // the frame's player is the puppet's character
		bool inGame{ true };
		bool dead{ false };       // Elden Ring's dead bit on that player
		bool guestAlive{ true };
		bool tarnished{ false };  // Hold::kTarnished
		bool stepAside{ false };  // Hold::kStepAside
		bool ladder{ false };     // Hold::kLadder
		bool allowed{ true };     // [Puppet] on, no other hold, addresses usable
	};
	inline const char* StopReason(const StopInput& a_in)
	{
		if (a_in.samePlayer && a_in.inGame && a_in.dead) {
			return "player dead";
		}
		if (!a_in.guestAlive) {
			return "guest lost";
		}
		if (a_in.tarnished) {
			return "Tarnished mode";
		}
		if (a_in.stepAside) {
			return "stepping aside: Elden Ring walks the Tarnished";
		}
		if (a_in.ladder) {
			return "a ladder: Elden Ring climbs it with the Tarnished";
		}
		if (!a_in.allowed) {
			return "disabled";
		}
		if (!a_in.samePlayer) {
			return "player object changed";
		}
		if (!a_in.inGame) {
			return "loading";
		}
		return nullptr;
	}

	// View + Start (er/Host.cpp UpdateTarnished) while a death is under way (Puppet::Dying(): Snake's kill went out, or the
	// Tarnished is dying, until the respawn): ignored. Tarnished mode switched on in a death's first frames stopped the puppet with
	// the Tarnished on screen (review of 0.6.0; the player's DualSense makes stray presses), and switched off it would hand Snake a
	// dead Tarnished. The chord works again after the respawn.
	inline bool ChordRefusedWhileDying(bool a_dying) { return a_dying; }

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
