#pragma once

// Self-calibration of the Elden Ring side's axis conventions (SkyCraft does the same with Skyrim's
// camera matrix): instead of trusting a guess, watch the game while the player still drives the
// Tarnished and vote.
//  - character forward: the Tarnished runs mostly where it faces, so the run direction vs
//    quat*(0,0,+1) says whether ER's local forward is +Z or -Z;
//  - camera rows: ER's follow camera looks at the player, so (player - camera) vs the +0x30 row says
//    whether that row is forward or backward; dot(right, up x forward) gives the handedness.
// An explicit [Axes] er_forward / [Camera] forward_row in the ini wins over the vote.

#include "Game.h"

#include <string>

namespace phantom::er
{
	namespace Calibration
	{
		void  Observe(const game::Player& a_player, float a_dt);  // only while ER drives the player
		float ErForward();     // +1 / -1
		float CamForwardRow(); // +1 / -1: sign of the +0x30 row relative to the view direction
		float CamHandedness(); // dot(right, cross(up, forward)) sign of ER's own camera
		bool  Locked();
		std::string Describe();
	}
}
