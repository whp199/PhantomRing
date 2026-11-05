#pragma once

// Quiet as a sniper buddy in Elden Ring (docs/research/quiet.md §3; protocol v18 kHostQuietLayer, buddyPost*, kActorQuietTarget /
// kActorQuietThreat, kGEvQuietShot; [Quiet] in PhantomRingHost.ini). MGSV runs Quiet (her standby, her aim and her shot at a
// stand-in soldier MGSV's binoculars marked: the guest, mgsv/Quiet.cpp); the host does what only Elden Ring can:
//  - her post: where she kneels in Elden Ring, picked from Elden Ring's ground with rays (er/QuietLogic.h: Cover 25-55 m round
//    Snake while hostiles are near, Near 8-16 m when calm, none -- unseen, cloaked -- while Snake travels), published as
//    buddyPost (MGSV world) for the guest to hold her MGSV body at; her draws are moved there whole (er/RenderScene.cpp). A change
//    of post in view is her cloak: a 0.3 s fade out, the new post, the guest's echo, a fade in;
//  - the pick (kActorQuietTarget on one ActorTable record): the hostile enemy at the view's centre her next Fire / Take aim takes,
//    seen from her post's sight point and from the camera, never a boss, never a non-hostile character; a reticle over him while
//    LB is held or the binoculars are up (white, Quiet's colour once MGSV holds a mark on his stand-in: kBuddyQuietMarked);
//  - her shot: kGEvQuietShot Hit fires from her post's sight point with Elden Ring's line of sight, by her rifle (Wicked lethal,
//    Guilty sleep, Sinful heavy), re-checked hostile / alive / not a boss when it lands;
//  - spotting: provisional marks on the hostiles she sees from her post within 60 m; "Quiet: Enemy spotted";
//  - the grace rest's seat (er/GraceRest.h) decides where she is drawn while Snake rests.
// Nothing at all runs while Quiet isn't out. [Quiet] enabled = false: 0.8.0's Quiet (kGEvQuietShot only logged, no layer bit).

#include "Math.h"

#include "phantomring_protocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	struct Frame;
	struct Anchor;

	namespace Quiet
	{
		void OnShot(const Frame& a_frame, const proto::GuestEvent& a_event);   // kGEvQuietShot (er/Events.cpp)
		void OnOrder(const Frame& a_frame, const proto::GuestEvent& a_event);  // kGEvBuddyOrder with b = 3 (Quiet)
		void Tick(const Frame& a_frame);  // FrameBegin, after the events (and after GraceRest::Tick)
		void Fill(proto::HostState& a_state, const Anchor& a_anchor);  // kHostQuietLayer and buddyPost* (MGSV world), every host frame
		void OnWorldShift(const Vec3& a_delta);
		// The ActorTable records that carry kActorQuietTarget / kActorQuietThreat (er/Actors.cpp; 0 = none). Any thread.
		std::uint64_t TargetHandle();
		std::uint64_t ThreatHandle();

		// Where her draws go this frame (er/RenderScene.cpp, er/Buddy.cpp): MGSV's own place (normal: the far-ground rule), moved
		// whole to an Elden Ring point (a post, the grace seat), or not drawn at all.
		struct Placement
		{
			enum class Kind : std::uint8_t
			{
				kNormal,
				kAt,
				kHidden,
			};
			Kind  kind{ Kind::kNormal };
			Vec3  feet;          // kAt: Elden Ring feet
			float alpha{ 1.0f }; // 1 solid .. 0 gone (the cloak fade)
			const char* why{ "" };
		};
		Placement Place();

		// Dev console: `quiet [status | on | off | all on|off | pick | test boss | post [now|none] | shoot [0xHANDLE|pick] [lethal|tranq|heavy]
		// | survey [label] | record on|off | set <key> <v>]`.
		std::string Console(const std::vector<std::string>& a_words, const Frame& a_frame);
		std::string Describe();
	}
}
