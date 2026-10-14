#include "Actors.h"

#include "Combat.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Link.h"
#include "Mem.h"
#include "Puppet.h"

#include <cstdio>

namespace phantom::er::Actors
{
	namespace
	{
		proto::ActorTable g_table;  // 16 KB: static, not on the task's stack
		bool              g_published{ false };

		std::uint8_t SafeTeam(const Frame& a_f) { return SafeRead<std::uint8_t>(a_f.player.chr + game::kChrTeam).value_or(0); }
	}

	void Publish(const Frame& a_f)
	{
		if (!Cfg().actors || !Puppet::Active() || !a_f.player) {
			if (g_published) {
				Link::Get().WriteActors(g_table, 0);
				g_published = false;
			}
			return;
		}
		const auto& anchor = Puppet::CurrentAnchor();
		const auto  myTeam = SafeTeam(a_f);
		std::uint32_t n = 0;
		for (const auto& c : game::Characters(static_cast<std::size_t>(Cfg().actorMax) + 1)) {
			if (c.chr == a_f.player.chr || c.distance > Cfg().actorRadius || n >= static_cast<std::uint32_t>(Cfg().actorMax)) {
				continue;
			}
			auto& r = g_table.actors[n++];
			r = {};
			r.handle = c.handle;
			r.flags = 0;
			if (c.isEnemy && c.team != myTeam && Combat::HostileToPlayer(a_f, c.team)) {
				r.flags |= proto::kActorHostile;
			}
			if (c.dead) {
				r.flags |= proto::kActorDead;
			} else if (Combat::Asleep(c.chr)) {
				r.flags |= proto::kActorAsleep;  // the guest's Fulton anchor stands a sleeping soldier in for him (docs/GUEST.md 20)
			}
			const Vec3 p = anchor.PointToMgsv(c.pos);
			r.pos[0] = p.x;
			r.pos[1] = p.y;
			r.pos[2] = p.z;
			const Vec3 f = anchor.DirToMgsv(ErForward(c.orientation, anchor.axes));
			r.yaw = anchor.axes.mgsvYawSign * Heading(f);
			r.radius = 0.5f;
			r.height = 1.8f;
			r.hpFrac = c.maxHp > 0 ? static_cast<float>(c.hp) / static_cast<float>(c.maxHp) : 0.0f;
			std::snprintf(r.name, sizeof(r.name), "npc %d", c.npcParam);
		}
		Link::Get().WriteActors(g_table, n);
		g_published = true;
	}
}
