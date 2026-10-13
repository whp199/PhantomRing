#pragma once

// Snake's placed explosives in Elden Ring (protocol v5): claymores, AP / sleep / stun mines, C4.
//
// MGSV's proximity mines only ever trigger on MGSV's own characters, and its sandbox has none: placed in Elden Ring
// they would never go off. So the host keeps each kGEvPlaced where it was placed (mapped through the anchor at that
// moment and set down on Elden Ring's ground; it stays there however far Snake walks or MGSV's treadmill warps him),
// draws it with a blinking light (er/RenderMarkers.h), and when a character the Tarnished may attack (Elden Ring's
// team relation, er/Combat.h) comes within its trigger radius sends kHEvDetonate once: MGSV sets it off, and its blast
// comes back as kGEvExplosion with pad[1] = the placed id, put here where the mine lies. C4 (trigger radius 0) only
// ever goes off from MGSV (Snake's own detonator). kGEvPlacedGone drops it.

#include "Host.h"
#include "Math.h"

#include <string>
#include <vector>

namespace phantom::er::Mines
{
	void OnPlaced(const Frame& a_frame, const proto::GuestEvent& a_event);
	void OnGone(const proto::GuestEvent& a_event);
	// A kGEvExplosion of one of our mines (pad[1]): blast where it lies. False: not ours.
	bool OnExplosion(const Frame& a_frame, const proto::GuestEvent& a_event);
	void Tick(const Frame& a_frame);  // FrameBegin: enemies in range -> kHEvDetonate; markers
	void OnPuppetStop();
	void OnWorldShift(const Vec3& a_delta);

	std::string Describe();
	// Dev console: `mines` (list) | `mines detonate <id>` (send kHEvDetonate as if an enemy had come near).
	std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
}
