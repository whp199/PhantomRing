#include "WorldShift.h"

#include "Explosion.h"
#include "Fulton.h"
#include "Game.h"
#include "Host.h"
#include "Log.h"
#include "Mines.h"
#include "Puppet.h"
#include "Rounds.h"
#include "Tasks.h"
#include "Throw.h"

#include <format>

namespace phantom::er::WorldShift
{
	namespace
	{
		const std::uint8_t* g_chr{ nullptr };  // the player read at 160
		Vec3                g_before;
		bool                g_have{ false };
		std::uint64_t       g_shifts{ 0 };
		Vec3                g_last;

		void OnBegin(float)
		{
			const auto p = game::MainPlayer();
			g_have = p && !Host::Current().loading;
			if (g_have) {
				g_chr = p.chr;
				g_before = game::Position(p);
			}
		}

		// The player moved since 160 with nothing else moving him: Elden Ring shifted its origin. a_where: which check saw it.
		void Follow(const char* a_where)
		{
			const auto p = game::MainPlayer();
			if (!p || p.chr != g_chr) {
				g_have = false;
				return;
			}
			const Vec3 now = game::Position(p);
			const auto d = ShiftOf(g_before, now);
			if (!d) {
				return;
			}
			g_before = now;  // the other check applies only what is left
			++g_shifts;
			g_last = *d;
			if (g_shifts <= 20) {
				Log::Info("world shift: Elden Ring moved its origin by ({:.2f}, {:.2f}, {:.2f}) (seen at {}); grenades and mines follow", d->x, d->y, d->z, a_where);
			}
			Throw::OnWorldShift(*d);
			Mines::OnWorldShift(*d);
			Rounds::OnWorldShift(*d);
			Puppet::OnWorldShift(*d);
			Explosion::OnWorldShift(*d);
			Fulton::OnWorldShift(*d);
		}

		void OnEnd(float)
		{
			if (g_have) {
				Follow("EndShiftWorldPosition");
			}
		}
	}

	void CheckBeforePhysics()
	{
		if (g_have && Host::Current().loading) {
			g_have = false;
		}
		if (g_have) {
			Follow("ChrIns_PrePhysics");
		}
		g_have = false;  // the window closes: from here on the puppet and the physics move the player
	}

	void Check()
	{
		if (g_have && Host::Current().loading) {
			g_have = false;  // a load moves the player for real (fast travel); our copies are dropped with the puppet
		}
		if (g_have) {
			Follow("FrameBegin");
		}
	}

	void Start()
	{
		RegisterTask(static_cast<TaskGroup>(160), &OnBegin, "BeginShiftWorldPosition");
		RegisterTask(static_cast<TaskGroup>(11), &OnEnd, "EndShiftWorldPosition");
	}

	std::string Describe()
	{
		return std::format("world shifts seen: {}{}", g_shifts, g_shifts ? std::format(" (last ({:.1f}, {:.1f}, {:.1f}))", g_last.x, g_last.y, g_last.z) : "");
	}
}
