#pragma once

// Motion v2 (re-backlog E1, [Puppet] motion = root_motion): Snake's displacement becomes the puppet's
// animation root motion, so Elden Ring's own character proxy moves it -- Havok resolves walls, slopes,
// steps, ledges and gravity exactly as for the Tarnished's own walk, and nothing is warped.
//
// PlayerCtrl vfunc 12 (0x3DE610, "ModifyRootMotion(ctrl, rm)") runs once per physics step with the
// character-local root-motion translation; ER then rotates it by the physics orientation (phys+0x50),
// divides by dt and hands it to CsChrProxy::SetVelocity. We take that slot on the player's PlayerCtrl
// only (VptrSwap), let the original run, then replace rm.xyz with our displacement rotated into the
// character's frame. The puppet task only adds to an accumulator; the hook takes it, so every metre
// of Snake's is applied exactly once whichever thread the behaviour update runs on.
//
// Statically verified (decompilation of the whole chain); UNVERIFIED live: that the hook runs once per
// frame, the conjugate direction, and any speed cap in the hknp integrator. Puppet falls back to the
// kinematic controller if the hook goes silent.

#include "Game.h"

#include <string>

namespace phantom::er
{
	namespace RootMotion
	{
		bool Install(const game::Player& a_player);  // swaps the player's PlayerCtrl vptr (verified first)
		void Remove(const game::Player& a_player);   // restores it (only on that same, live player)
		void Add(const Vec3& a_havokDisplacement);   // horizontal metres for the next physics step(s)
		void Clear();
		// The hook ran within a_withinMs (false before its first call).
		[[nodiscard]] bool Alive(double a_withinMs);
		[[nodiscard]] bool Installed();
		[[nodiscard]] const void* FakeVtable();  // MainPlayer() must accept the swapped PlayerCtrl
		std::string Describe();
	}
}
