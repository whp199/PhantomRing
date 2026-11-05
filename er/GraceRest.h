#pragma once

// Resting at a Site of Grace with the buddy (docs/research/quiet.md §3.9; protocol v18 kHostAtGrace, graceCentre / graceSeat /
// graceSeatYaw / graceSeq, the guest's kBuddyCanRest / kBuddySeated / graceSeqSeated; [Grace] in PhantomRingHost.ini). The rules
// are er/GraceLogic.h; this is the game side:
//  - the observed black (U12, found live 2026-11-04): Elden Ring's CSFadeImp holds nine CSFD4FadePlates (Title, MapIn, InGame,
//    Cutscene, InCutscene, Ending, Event, InGameChroma, InGameBokeh); the one named "InGame" fades the screen to black for a rest
//    (0.75 s out, ~1.4 s held, 0.5 s in), for Pass time and for loads. The plate is found by its RTTI and its name, never by an
//    index; anything else ([Grace] rest_dark = none / luma, or the plate not as verified) means no grace rest, logged once;
//  - U12's record: every rest (any buddy, layer on or off) logs when its black started and ended after the press and when the HUD
//    hid, while the plate reads;
//  - the seat: armed by a rest (kHEvRested 0) with Quiet out, the guest's kBuddyCanRest, [Grace] buddy_rest, [Quiet] enabled and
//    posts, not in Tarnished mode ([Grace] tarnished); on the first black frame the grace's centre (the AEG099_060 asset within
//    2.5 m, else 1.25 m along the player's facing) and the seat search (a ray budget a frame) run; the seat is published inside
//    the black before the deadline; the buddy is drawn there only once the guest echoes it inside the black (er/Quiet.h asks
//    AtSeat / Hide). Nothing of this moves anything without an observed black: a rest that turns into a talk gets no seat.

#include "Math.h"

#include "phantomring_protocol.h"

#include <string>
#include <vector>

namespace phantom::er
{
	struct Frame;
	struct Anchor;

	namespace GraceRest
	{
		void OnRest(const Frame& a_frame);  // kHEvRested 0 (er/Host.cpp UpdateRest; a respawn never comes here)
		void OnDialog();                    // an Elden Ring dialog started (er/Host.cpp): a seat being taken or held is cancelled
		void Tick(const Frame& a_frame);    // FrameBegin, after the events: the fade, the gate, the search, the echo
		// HostState: kHostAtGrace and the grace fields (MGSV world through a_anchor), every host frame.
		void Fill(proto::HostState& a_state, const Anchor& a_anchor, bool a_atGrace);
		bool AtSeat();      // the buddy is drawn at the seat (the guest echoed it)
		float SeatAlpha();  // its alpha there: 1, or the fade-in when the echo came after the black (she settled late)
		bool Hide();        // the buddy is not drawn (the seat published, not echoed in time; a dialog)
		Vec3 Seat();        // Elden Ring feet
		// The seat she was drawn at this rest, kept after he leaves (until the next rest): she gets up from it (er/Quiet.cpp).
		bool LastSeat(Vec3& a_feet, float& a_heading);
		float SeatHeading();  // Elden Ring heading toward the grace
		bool Dark();        // the observed black this frame
		bool FadeUsable();  // the InGame plate reads as verified
		void OnWorldShift(const Vec3& a_delta);
		// Dev console: `grace [status | fade | on | off | survey | set <key> <v>]`.
		std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
		std::string Describe();
	}
}
