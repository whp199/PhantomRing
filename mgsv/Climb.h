#pragma once
// Snake climbs Elden Ring's ladders and ledges with MGSV's own moves (protocol v15: kHEvClimb / kHEvMantle in,
// kGuestClimbing / kGuestMantling out; docs/GUEST.md 22). The plan and why Snake is staged at real MGSV geometry:
// ClimbLogic.h.
//
// Game thread (Frame.cpp): host events queue a move; Update runs it once per frame -- the stand placements and the ladder's
// loop placements through the treadmill's native placement (hold::Place, + WarpLift as for every placement), MGSV's action
// button and the left stick on the pad MGSV reads (the guest owns the pad while a move plays), the pace against Elden Ring's
// speed. While a move plays Snake is "staged": away from the sandbox on purpose -- the odometer ignores him (Frame.cpp keeps
// it still), no treadmill, jump or dive, D-Walker is not whistled after him, the Fulton anchor and proxies stay out, and
// MGSV's HUD layer keeps its last frame (its "Y: climb" prompt and ladder icon never reach Elden Ring). When it is over he is
// placed back where he stood on the sandbox, facing the way he faced, and MGSV's camera is turned back.

#include "Logic.h"
#include "phantomring_protocol.h"

#include <string>
#include <string_view>

namespace phantom::guest::camera
{
	struct Sample;
}
namespace phantom::guest::state
{
	struct LuaView;
}

namespace phantom::guest::climb
{
	// Frame.cpp's host-event loop: kHEvClimb / kHEvMantle (any other type is ignored).
	void OnHostEvent(const proto::HostEvent& a_event);

	struct Result
	{
		bool climbing{ false };  // kGuestClimbing this frame
		bool mantling{ false };  // kGuestMantling this frame
		bool placed{ false };    // Snake was placed this frame (the odometer stays still)
	};
	// Frame.cpp, every player frame, after the host events and before the jump and the hold. a_dt: seconds since the last frame.
	Result Update(const camera::Sample& a_cam, const logic::Vec3& a_root, float a_yaw, const state::LuaView& a_lua, bool a_ready, double a_dt);

	// Any thread: a move is under way (Snake may be away from the sandbox). Also state::Staged().
	[[nodiscard]] bool Active();
	// Game thread, while a move plays: the camera to publish -- the one Snake had when it began, at the same place relative to his
	// root (MGSV's own swings round the ladder or the wall), so a host that follows MGSV's camera keeps its view. False otherwise.
	bool PublishedCamera(const logic::Vec3& a_root, float a_pos[3], float a_quat[4]);
	// Where Snake stood on the sandbox when the move began (the buddy is published relative to it meanwhile).
	logic::Vec3 Home();

	// `!climb up|down [speed] | off`, `!mantle <cm> [vault] [secs]` (queued for the game thread, as the host events are); `!climb` /
	// `!mantle` alone: the state.
	std::string Command(std::string_view a_args, bool a_mantle);
	std::string Describe();
}
