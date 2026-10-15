#pragma once
// CQC holds and throws on Elden Ring enemies (ROADMAP 2.2 tier 1, docs/GUEST.md 21): the game thread feeds MGSV's pad bits and
// Snake's state to the intent tracker (CqcLogic.h) every frame and sends what it starts as protocol v13 kGEvCqc -- v0 = Snake's
// feet and v1 = his facing (MGSV world), b = the grab's reach (mm), pad[0] = MGSV's CQC attack id of the moment (logged by the
// host, for mapping MGSV's real holds live), pad[1] = 0 (tier 1 knows no proxy target). The host picks the Elden Ring enemy.

#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::guest::cqc
{
	struct FrameInput
	{
		bool          ready{ false };        // the sandbox runs and Snake is controllable
		bool          riding{ false };
		std::uint32_t luaFlags{ 0 };         // state::LuaFlags
		std::uint32_t buttons{ 0 };          // PlayerVars.scannedButtonsDirect
		std::uint64_t frame{ 0 };
		float         root[3]{};             // Snake's feet, MGSV world
		float         yaw{ 0 };              // radians, 0 = +Z
	};

	// Game thread, once per frame after combat::PollMelee.
	void Update(const FrameInput& a_in);

	// `!cqc`: state and counts; `!cqc <grab|hold|throw|choke|interrogate|release>` sends that event by hand from Snake's last
	// place (for testing the host without MGSV's controls).
	std::string Console(std::string_view a_args);
}
