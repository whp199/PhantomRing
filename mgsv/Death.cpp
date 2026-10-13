#include "Death.h"

#include "Config.h"
#include "Log.h"
#include "Pad.h"
#include "State.h"

#include <windows.h>
#include <xinput.h>

#include <format>
#include <mutex>

namespace phantom::guest::death
{
	namespace
	{
		enum class Phase
		{
			kAlive,
			kFalling,  // the dive (or the stance change) is playing
			kDown,     // lying prone, pad cut off, until the respawn
			kRising,   // standing up; the pad comes back when it is done
		};

		// MGSV PlayerStance values (VERIFIED live: PlayerStance.STAND 1, SQUAT 2, CRAWL 3) for PR.Run 'stance'.
		constexpr int       kStand = 1;
		constexpr int       kCrawl = 3;
		constexpr ULONGLONG kFallMs = 1200;    // a dive lands in ~1 s; then make sure he is prone
		constexpr ULONGLONG kRiseMs = 900;     // prone -> stand
		constexpr ULONGLONG kStuckMs = 120000;  // no respawn and the host no longer dead: get up anyway

		Phase       g_phase{ Phase::kAlive };
		ULONGLONG   g_since{ 0 };
		bool        g_hostWasDead{ false };
		std::mutex  g_lock;
		std::string g_text{ "alive" };

		void Enter(Phase a_phase, ULONGLONG a_now, std::string_view a_why)
		{
			g_phase = a_phase;
			g_since = a_now;
			std::scoped_lock lock(g_lock);
			g_text = std::format("{} ({})", a_phase == Phase::kAlive ? "alive" : a_phase == Phase::kFalling ? "falling" : a_phase == Phase::kDown ? "down" : "rising", a_why);
			Log::Info("death: {}", g_text);
		}
	}

	void Update(const Facts& a_f)
	{
		if (!Cfg().deathCollapse) {
			return;
		}
		const ULONGLONG now = ::GetTickCount64();
		const ULONGLONG in = now - g_since;
		if (a_f.hostDead) {
			g_hostWasDead = true;
		}
		switch (g_phase) {
		case Phase::kAlive:
			if ((a_f.died || a_f.hostDead) && a_f.ready) {
				pad::SetBlocked(true);
				// His own dive when he stands (X: a forward dive into prone); crouched, prone or riding: the stance.
				const bool standing = (a_f.luaFlags & state::kLStand) && !a_f.riding;
				if (standing) {
					pad::Pulse(XINPUT_GAMEPAD_X, 150);
				} else {
					state::QueueCommand(std::format("stance {}", kCrawl));
				}
				g_hostWasDead = a_f.hostDead;
				Enter(Phase::kFalling, now, a_f.died ? (standing ? "life gone: dive" : "life gone: prone") : (standing ? "Elden Ring player dead: dive" : "Elden Ring player dead: prone"));
			}
			break;
		case Phase::kFalling:
			if (in > kFallMs) {
				if (!(a_f.luaFlags & state::kLCrawl)) {
					state::QueueCommand(std::format("stance {}", kCrawl));
				}
				Enter(Phase::kDown, now, "lying down until the respawn");
			}
			break;
		case Phase::kDown:
			{
				// Up on the respawn; also if the host went away, or it was dead and is not any more and nothing
				// came for a long time (a lost event must not leave the player without a pad).
				const bool lost = !a_f.hostAlive || (in > kStuckMs && !a_f.hostDead);
				if (a_f.respawned || lost || (g_hostWasDead && !a_f.hostDead && in > 3000)) {
					state::QueueCommand(std::format("stance {}", kStand));
					g_hostWasDead = false;
					Enter(Phase::kRising, now, a_f.respawned ? "respawn" : lost ? "no host / timeout" : "host alive again");
				}
				break;
			}
		case Phase::kRising:
			if (in > kRiseMs) {
				pad::SetBlocked(false);
				Enter(Phase::kAlive, now, "pad back");
			}
			break;
		}
		if (a_f.respawned && g_phase == Phase::kFalling) {
			// Respawned before he even landed (instant respawn): get up right away.
			state::QueueCommand(std::format("stance {}", kStand));
			Enter(Phase::kRising, now, "respawn while falling");
		}
	}

	bool Down() { return g_phase != Phase::kAlive; }

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return g_text;
	}
}
