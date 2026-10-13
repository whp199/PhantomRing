#pragma once
// Snake's death, MGSV's way, while Elden Ring runs YOU DIED (docs/GUEST.md "Death"). MGSV itself never
// dies here (that would be its mission-failed / game-over flow): when the guest's life clamp latches
// kGEvDied, or the host reports kHostPlayerDead (a fall, anything Elden Ring killed the Tarnished for),
// Snake throws himself to the ground (his own dive, X, when standing; otherwise the prone stance) and
// lies there with the player's pad cut off until kHEvRespawn; then he gets up (stand stance) and the pad
// comes back. The host keeps drawing him ([Render] death_visual = keep).

#include <cstdint>
#include <string>

namespace phantom::guest::death
{
	struct Facts
	{
		bool          ready{ false };       // sandbox ready, in gameplay
		bool          died{ false };        // the guest's life clamp latched this frame (kGEvDied sent)
		bool          hostDead{ false };    // HostState kHostPlayerDead
		bool          respawned{ false };   // kHEvRespawn arrived this frame
		bool          hostAlive{ false };   // the host is attached and heartbeating
		std::uint32_t luaFlags{ 0 };        // PR.Tick flags (stance)
		bool          riding{ false };
	};

	// Once per player-camera frame (Frame.cpp).
	void Update(const Facts& a_facts);

	// Snake is collapsing or lying down (the pad is cut off).
	[[nodiscard]] bool Down();

	std::string Describe();
}
