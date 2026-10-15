#pragma once

// The host's frame: what every subsystem needs to know about this Elden Ring frame, sampled once at
// FrameBegin so all tasks of the frame agree (guest snapshot, player, menu/loading state).
//
// Task schedule (ER task groups, docs/research/elden-ring-runtime-re.md §3.9):
//   FrameBegin (0)                link heartbeat, guest snapshot, menus, console, guest events,
//                                 puppet start/stop, input mode
//   ChrIns_PrePhysics (78)        puppet motion (kinematic controller -> SetPosition/SetYaw)
//   DrawParamUpdate (107)         camera (CSCamera.pers_cam_1 <- MGSV camera); ini task_group
//   WorldChrMan_PostPhysics (117) HP/hurt/respawn, actors, buddy, debug draw, HostState

#include "Game.h"
#include "phantomring_protocol.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::er
{
	struct Frame
	{
		std::uint64_t     index{ 0 };
		float             dt{ 1.0f / 60.0f };
		bool              guestAlive{ false };
		bool              guestReady{ false };  // alive, attached, GuestState read, kGuestReady
		bool              guestRead{ false };   // GuestState read this frame (guest is this frame's, ready or not)
		proto::GuestState guest{};
		game::Player      player;
		bool              inGame{ false };
		bool              loading{ false };
		bool              dead{ false };
		bool              menuOpen{ false };     // learned UI states / Start latch, or CSMenuMan's blocking-menu field
		bool              modalMenu{ false };    // CSMenuMan+0x798 (re-backlog E4)
		bool              cutscene{ false };     // CSRemo playing (re-backlog E4): ER owns the player and the pad
		std::int32_t      prompt{ -1 };          // ActionButtonParam id of ER's interaction prompt, -1 = none (E6)
		bool              interacting{ false };  // an ER interaction Snake started with the prompt button (er/Interaction.h); in menuOpen too
		bool              hudHidden{ false };    // ER's in-game HUD is hidden (grace menu, talk, revival choice: er/Host.cpp HudHidden)
		bool              hudKnown{ false };     // hudHidden means something: the HUD was seen visible since the last load
		bool              dying{ false };        // from the death until Elden Ring's respawn (Puppet::Dying; the dead bit clears early)
	};

	namespace Host
	{
		bool         Start();  // after system init: registers the tasks
		const Frame& Current();
		std::string  StatusLine();
		// Tarnished mode (View + Start held together, er/Chord.h): the player plays the Tarnished, Snake parks.
		bool        Tarnished();
		std::string TarnishedCommand(std::string_view a_arg);  // console `tarnished [on|off|toggle]`
	}
}
