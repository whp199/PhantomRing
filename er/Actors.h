#pragma once

// ActorTable (host -> guest): Elden Ring characters near the puppet, mapped into MGSV world through
// the anchor so the guest can mark them / aim-assist / place proxies (DESIGN.md §8.1 later phases).
// Hostility, radius and height are approximations until phase 3 (UNVERIFIED: team-type semantics).

namespace phantom::er
{
	struct Frame;

	namespace Actors
	{
		void Publish(const Frame& a_frame);  // WorldChrMan_PostPhysics
	}
}
