#pragma once

// Guest events (MGSV -> ER), consumed at FrameBegin:
//  kGEvFire / kGEvBuddyFire  traced in ER and resolved against characters: damage, poise, sleep (er/Combat.h)
//  kGEvMelee                 CQC strike sphere + arc in front of Snake -> the nearest enemy (er/Combat.h)
//  kGEvExplosion             blast damage around the mapped centre (er/Combat.h)
//  kGEvDied                  the puppet dies the game's way (Puppet::OnGuestDied)
//  kGEvRumble                forwarded to the real controller
//  kGEvLog                   logged
// Host events (kHEvHurt / kHEvRespawn / kHEvWarped) are produced by er/Puppet.cpp.

namespace phantom::er
{
	struct Frame;

	namespace Events
	{
		void Poll(const Frame& a_frame);  // also runs Combat::Tick (pending hit verification)
		unsigned long long Count();
	}
}
