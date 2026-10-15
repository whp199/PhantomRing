#pragma once

// Projectile meshes (roadmap 5.4): Snake's grenade and rocket drawn in Elden Ring with MGSV's own meshes.
//
// The host flies its own copies of what Snake throws and fires (er/Throw.h, er/Rounds.h) through Elden Ring's world, and
// used to draw them as a small olive ball (a grenade) or a small dark ball (a rocket). The guest can tag the MGSV draws of a
// projectile it just released (protocol v13 kOwnerProjectile, mgsv/ProjectileTag.h, [Capture] ProjectileMeshes). Here:
//  - every release (kGEvThrow / an explosive kGEvFire) is noted with its MGSV-world direction (NoteRelease);
//  - a release's tagged draws become that item's template (keyed by the throw's equip id, the round's bullet id), turned into
//    the projectile's own frame (er/ProjectileLogic.h) -- only once two lists 3+ MGSV frames apart show the same meshes
//    flying away along the release (projectile::Confirms): Snake's gear or a prop the guest mis-tagged stays still. The
//    first good template is kept; it is replaced only when kReplaceAfter later learns in a row agree on other meshes. Once
//    its meshes are uploaded its real size is measured (projectile::Across): too big for a grenade / rocket = refused;
//  - each frame the flown copies (Set) are drawn with their template, turned along their velocity (a grenade tumbling about
//    it; one that slows keeps its last pose, projectile::Pose), through the same renderer as Snake. Throw and Rounds leave
//    out their ball only for a copy that was really placed in the last scene (Drawn), and keep their exhaust/trail. No
//    template yet (the first throw of an item, a guest that doesn't tag, a mesh not uploaded): the ball as before.
// kOwnerProjectile draws are never drawn where MGSV has them (its grenade flies over MGSV's airport, not Elden Ring's).
// [Render] projectile_meshes = false (the default until verified live): nothing learned or placed; tagged draws go back to
// where the guest put them before v13 (Snake's within 2.5 m of his root, else out: projectile::HandBack).

#include "Math.h"

#include <cstdint>
#include <string>
#include <vector>

namespace phantom::er
{
	struct RenderSnapshot;

	namespace Projectiles
	{
		enum class Source : std::uint8_t
		{
			kThrow,
			kRounds,
			kCount,
		};
		inline std::uint64_t KeyOf(Source a_s, std::uint32_t a_id) { return (std::uint64_t(a_s) + 1) << 32 | a_id; }

		struct Flying
		{
			std::uint64_t key{ 0 };  // what it is: KeyOf(source, equip / bullet id)
			std::uint32_t id{ 0 };   // which copy (Throw's grenade id, Rounds' launch count): its pose is kept by it
			Vec3          pos;       // ER world
			Vec3          vel;       // ER world (m/s)
			float         age{ 0 };  // s since the release (a thrown one tumbles with it)
		};

		// Task thread: a projectile of a_key left Snake, flying along a_erDir (ER world).
		void NoteRelease(std::uint64_t a_key, const Vec3& a_erDir);
		// Task thread: this frame's flown copies of one source (replaces what it gave last frame).
		void Set(Source a_source, std::vector<Flying> a_flying);
		// Was copy a_id of a_source drawn with its MGSV mesh in the scene of Elden Ring frame a_erFrame or the one before (its
		// owner leaves out the ball)?
		bool Drawn(Source a_source, std::uint32_t a_id, std::uint64_t a_erFrame);
		// RenderScene::Capture (task thread), also when MGSV's list is missing: learn from and drop this list's kOwnerProjectile
		// draws, add the flown copies'. False: nothing left to draw.
		bool Apply(RenderSnapshot& a_snapshot);
		void OnPuppetStop();
		std::string Describe();
	}
}
