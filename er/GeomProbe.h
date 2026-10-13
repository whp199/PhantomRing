#pragma once

// Destructible map assets around the player, for tests and diagnostics (console `geom`): Elden Ring's crates, pots,
// barrels and carts are CSWorldGeomIns of CSWorldGeomMan, each with its AssetEnvironmentGeometryParam row (hp > 0 = it can
// break; bits_3b bit0 = it breaks when the player's body runs into it). Layouts: fromsoftware-rs cs/world_geom_man.rs,
// shared/stl (MSVC std::map / std::vector) [V-src]; checked live, see docs/HOST.md "Breakables".

#include "Host.h"
#include "Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er::GeomProbe
{
	struct Asset
	{
		std::uint8_t* ins{ nullptr };
		std::string   name;           // the MSB part's name ("AEG099_090_9000")
		std::int16_t  hp{ 0 };        // its param row's hp (-1 = can't break)
		std::uint8_t  flags{ 0 };     // param bits_3b: bit0 breaks by the player's body, bit1 by an enemy's
		Vec3          pos;            // Elden Ring world (Havok) when found
		bool          posValid{ false };
		bool          broken{ false };
	};

	// Every asset of the loaded blocks (a_breakableOnly: hp > 0) within a_radius of a_centre whose position is known.
	std::vector<Asset> Near(const Vec3& a_centre, float a_radius, bool a_breakableOnly);

	std::string Command(const std::vector<std::string>& a_words, const Frame& a_frame);
}
