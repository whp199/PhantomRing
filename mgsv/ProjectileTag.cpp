#include "ProjectileTag.h"

#include "Config.h"
#include "Log.h"
#include "ProjectileLogic.h"

#include <array>
#include <atomic>
#include <format>
#include <mutex>

namespace phantom::guest::projectile
{
	namespace
	{
		// A few releases at once: a burst of grenade-launcher rounds, a grenade thrown right after a rocket.
		std::mutex             g_lock;
		std::array<Release, 8> g_releases{};
		std::size_t            g_next{ 0 };
		std::atomic<std::uint64_t> g_newest{ 0 };  // frame of the newest release (a cheap "nothing recent" test without the lock)
		std::atomic<std::uint64_t> g_throws{ 0 }, g_rounds{ 0 }, g_tagged{ 0 };
		std::atomic<bool>          g_loggedFirst{ false };

		void Note(std::uint64_t a_frame, const float a_p[3], const float a_v[3], bool a_round)
		{
			if (!Cfg().projectileMeshes || !a_frame) {
				return;
			}
			std::scoped_lock lock(g_lock);
			auto& r = g_releases[g_next++ % g_releases.size()];
			r.frame = a_frame;
			r.round = a_round;
			for (int i = 0; i < 3; ++i) {
				r.p0[i] = a_p[i];
				r.v0[i] = a_v[i];
			}
			g_newest.store(a_frame, std::memory_order_relaxed);
		}
	}

	void NoteThrow(std::uint64_t a_frame, const float a_p0[3], const float a_vel[3])
	{
		Note(a_frame, a_p0, a_vel, false);
		g_throws.fetch_add(1, std::memory_order_relaxed);
	}

	void NoteRound(std::uint64_t a_frame, const float a_muzzle[3], const float a_dir[3])
	{
		Note(a_frame, a_muzzle, a_dir, true);
		g_rounds.fetch_add(1, std::memory_order_relaxed);
	}

	bool Tag(std::uint64_t a_frame, const float a_at[3])
	{
		if (!Cfg().projectileMeshes) {
			return false;
		}
		const TagParams p;
		// Every rigid G-buffer draw of 24 recording threads asks: no lock unless a release is recent.
		const auto newest = g_newest.load(std::memory_order_relaxed);
		if (!newest || (a_frame > newest && a_frame - newest > p.windowFrames)) {
			return false;
		}
		std::scoped_lock lock(g_lock);
		for (const auto& r : g_releases) {
			if (Matches(r, a_frame, a_at, p)) {
				g_tagged.fetch_add(1, std::memory_order_relaxed);
				if (!g_loggedFirst.exchange(true)) {
					Log::Info("projectile: first {} draw tagged at {:.2f},{:.2f},{:.2f} ({} frames after the release) -> kOwnerProjectile",
						r.round ? "round" : "thrown", a_at[0], a_at[1], a_at[2], a_frame - r.frame);
				}
				return true;
			}
		}
		return false;
	}

	std::string Describe()
	{
		return std::format("projectile meshes {}: {} throws, {} explosive rounds noted, {} draws tagged kOwnerProjectile", Cfg().projectileMeshes ? "on" : "off",
			g_throws.load(), g_rounds.load(), g_tagged.load());
	}
}
