#pragma once

// Snake's decoys in Elden Ring (protocol v13: kGEvPlaced with a = kAmmoDecoy; roadmap 2.8). UNVERIFIED live.
//
// MGSV's decoys only fool MGSV's soldiers, and its sandbox has none. So the host keeps each one where Snake set it (mapped
// through the anchor at that moment and set down on Elden Ring's ground, like a mine: er/Mines.h), draws it as a
// soldier-shaped balloon that blows up and, popped, collapses (er/RenderMarkers.h), and makes Elden Ring's enemies fall for
// it the box poster's way (er/Stealth.h): a hostile that can see a standing decoy within range stops and stares at it for a
// while (ChrIns+0x538 no attack / no move), then goes on. An active decoy is heard (no sight line). Snake slips past
// behind them. Elden Ring's AI has no notion of a lure, so nobody walks up to it (that needs its AI's sound / target
// research: docs/ROADMAP.md 2.8); an enemy that comes close on his own knocks it over: it pops (kHEvDetonate: the guest
// sends kGEvPlacedGone and never announces it again) and a shock decoy stuns who is near. kGEvPlacedGone (Snake picked it
// up in MGSV) drops it.

#include "Host.h"
#include "Math.h"

#include <string>
#include <vector>

namespace phantom::er::Decoys
{
	void OnPlaced(const Frame& a_frame, const proto::GuestEvent& a_event);  // kGEvPlaced with a = kAmmoDecoy
	void OnGone(const proto::GuestEvent& a_event);                          // every kGEvPlacedGone (not ours: ignored)
	void Tick(const Frame& a_frame);  // FrameBegin, after the events: stares, pops, the drawn balloons
	void OnPuppetStop();
	void OnWorldShift(const Vec3& a_delta);

	std::string Describe();
	// Dev console: `decoys` (list) | `decoys add [m] [plain|active|shock]` (a test decoy m ahead of the Tarnished, no MGSV) |
	// `decoys pop <id>`.
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
}
