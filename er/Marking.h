#pragma once

// MGSV's binocular marking on Elden Ring's characters (ROADMAP 2.6; protocol v13 GuestState kGuestBinoculars, docs/HOST.md
// "Marking"). While Snake looks through his binoculars (the guest sets the flag from MGSV's PlayerStatus BINOCLE; Elden
// Ring's camera is then the binoculars' view, er/CameraDriver.h, zoom included), every Elden Ring character near the centre
// of the view, within [Marking] range and in plain sight (Elden Ring's own ray cast from the camera), is marked after
// [Marking] dwell seconds of looking, as MGSV marks its soldiers.
//
// A marked character carries an MGSV-style mark until the next load screen (fast travel, a death, the title, another save --
// as MGSV's last a mission): a triangle pointing down over his head (red for a hostile, white for anyone else) and his
// distance from Snake in metres, the same size on screen at any distance, seen through walls (the world markers,
// er/RenderMarkers.h, with a depth slack larger than any scene). It fades out when he dies and hides while Elden Ring does not
// render him. Marks show only while Snake plays (not for the Tarnished, not in Elden Ring's or MGSV's menus, talks,
// cutscenes, loads, death); Elden Ring's HUD hiding doesn't hide them (its "Auto" HUD hides while Snake scouts).
//
// Elden Ring itself is only read (its character list, the camera) and ray cast; nothing is written. The pure rules are
// er/MarkingLogic.h. UNVERIFIED live: the guest's BINOCLE flag in real play, the look of the marks over Elden Ring's frame.

#include <string>
#include <string_view>

namespace phantom::er
{
	struct Frame;

	namespace Marking
	{
		void Tick(const Frame& a_frame);  // FrameBegin, after the events (er/Events.cpp)
		std::string Describe();
		// `mark [status | binoculars on|off|auto | nearest | clear]`: binoculars on = as if Snake looked through them (tests without
		// MGSV's flag); nearest = mark the nearest character now; clear = forget every mark.
		std::string Command(const Frame& a_frame, std::string_view a_args);
	}
}
