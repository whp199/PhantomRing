// The guest's side of Dive.h: the player's X presses and MGSV's PlayerStatus bits in, kGuestDiving out, each dive logged
// with the sign that confirmed it (UNVERIFIED live which one MGSV gives).
#include "Config.h"
#include "Dive.h"
#include "Log.h"
#include "Menu.h"
#include "Pad.h"
#include "State.h"

#include <windows.h>
#include <xinput.h>

#include <algorithm>
#include <mutex>

namespace phantom::guest::dive
{
	namespace
	{
		std::mutex g_lock;
		DiveWatch  g_watch;
	}

	bool Observe(bool a_ready, bool a_riding, std::uint32_t a_luaFlags, float a_speed)
	{
		const auto& c = Cfg();
		if (!c.diveFlag) {
			return false;
		}
		constexpr std::uint32_t kBusy = state::kLCqc | state::kLFall | state::kLDead | state::kLMenu | state::kLDemo | state::kLCbox | state::kLCarry;
		Facts f;
		f.nowMs = ::GetTickCount64();
		f.xPresses = pad::Presses(XINPUT_GAMEPAD_X);
		f.eligible = a_ready && !a_riding && !(a_luaFlags & kBusy) && !menu::PauseOpen() && !state::Parked();
		f.standing = (a_luaFlags & state::kLStand) != 0;
		f.crawl = (a_luaFlags & state::kLCrawl) != 0;
		f.normal = (a_luaFlags & state::kLNormal) != 0;
		f.speed = a_speed;
		Tunables t;
		t.confirmMs = static_cast<std::uint32_t>(std::max(c.diveConfirmMs, 0));
		std::scoped_lock lock(g_lock);
		DiveWatch::Event ev{};
		const bool       diving = g_watch.Update(f, t, &ev);
		switch (ev) {
		case DiveWatch::Event::kConfirmed:
			Log::Info("dive: X -> diving (confirmed by '{}' {} ms after X, {:.1f} m/s) -> kGuestDiving", g_watch.Sign(), g_watch.SignMs(), a_speed);
			break;
		case DiveWatch::Event::kUnconfirmed:
			if (g_watch.Unconfirmed() <= 10 || g_watch.Unconfirmed() % 50 == 0) {
				Log::Info("dive: X while standing but no sign of a dive within {} ms (flags {:#x}, {:.1f} m/s) -- no kGuestDiving (#{})", t.confirmMs, a_luaFlags,
					a_speed, g_watch.Unconfirmed());
			}
			break;
		case DiveWatch::Event::kEnded:
			Log::Debug("dive: over ({:.1f} m/s, flags {:#x})", a_speed, a_luaFlags);
			break;
		default:
			break;
		}
		return diving;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		return g_watch.Describe() + (Cfg().diveFlag ? std::string() : std::string(" ([Motion] DiveFlag = false)"));
	}
}
