#pragma once
// Fulton of a sleeping Elden Ring enemy (protocol v12 kGEvFulton; docs/GUEST.md 20.4) without moving MGSV's soldiers.
//
// ActorProxy v0 (20) can't place a soldier where an Elden Ring enemy lies: the player location manager it was built on knows
// only Snake (live 2026-10-14, `!loc` on a TppSoldier2 id: none) and TppSoldier2's command handler has no Warp. What does work,
// live: an MGSV soldier enabled (SetEnabled true) and a frame later put to sleep (ChangeLifeState state 3) lies where he stood,
// and MGSV offers Snake its own Fulton next to him (Y held: "Fulton 100% x48"), the balloon lifts him (his GetPosition y rises
// metres a second). So the *Snake* side moves: when the host reports a hostile, sleeping Elden Ring character (ActorTable
// kActorAsleep) within [Fulton] Reach of Snake, an "anchor" soldier near the sandbox is woken and put to sleep, and Snake is
// placed (the treadmill's native placement, the odometer rebased: Elden Ring sees no jump) so that the soldier lies exactly
// where that enemy lies relative to him. MGSV then shows its Fulton prompt for the enemy at Snake's feet; when the soldier rises,
// the guest sends kGEvFulton for that enemy's handle (the host lifts him out of Elden Ring) and Snake goes back to the sandbox.
// Walking away, the enemy waking up or dying gives the soldier back (disabled again) and Snake back to the sandbox.
//
// Everything Lua runs on MGSV's Lua thread (Pump's ready step); the placements run on the game thread (Frame.cpp's hold step).

#include "Logic.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace phantom::proto
{
	struct ActorRecord;
}

namespace phantom::guest::fulton
{
	namespace plan
	{
		using logic::Vec3;

		// The sleeping, hostile, living Elden Ring character nearest Snake within a_reach (horizontal), not one already fultoned;
		// -1 = none.
		int PickSleeper(const proto::ActorRecord* a_records, std::uint32_t a_count, const Vec3& a_snake, float a_reach,
			const std::vector<std::uint64_t>& a_done);

		// Where Snake must stand so that a soldier lying at a_soldier is where the enemy at a_enemy is relative to him now
		// (horizontal offset kept; Snake's height = a_ground, the soldier's floor).
		Vec3 SnakeSpot(const Vec3& a_snake, const Vec3& a_enemy, const Vec3& a_soldier, float a_ground);

		// "id:x,y,z;id:x,y,z" (PhantomRing.FultonCandidates) -> ids and standing positions, in the order given.
		struct Candidate
		{
			std::uint16_t id{ 0xFFFF };
			Vec3          pos;
		};
		std::vector<Candidate> ParseCandidates(std::string_view a_text);
	}

	// Pump (Lua thread), every Tick while the sandbox is ready. a_call runs a Lua chunk and returns its first result.
	void LuaStep(std::uint64_t a_nowMs, const std::function<std::string(const std::string&)>& a_call);
	// The sandbox was set up again (new mission load): forget the candidates and anything under way.
	void Reset(const char* a_why);

	// Frame.cpp (game thread): a placement of Snake to do now, if one is asked for (pos = his feet; centre = the treadmill's new
	// centre), and its outcome.
	struct Placement
	{
		logic::Vec3 pos;
		logic::Vec3 centre;
		const char* why{ "" };
	};
	std::optional<Placement> TakePlacement();
	void                     PlacementDone(bool a_ok, const logic::Vec3& a_shift);

	// Capture (any thread): the anchor soldier's draws are never Snake's (he lies within Snake's 2.5 m owner radius).
	bool ExcludesDraw(const float a_drawPos[3], const float a_snakeRoot[3]);
	[[nodiscard]] bool Anchored();
	std::string Describe();  // `!fulton`
}
