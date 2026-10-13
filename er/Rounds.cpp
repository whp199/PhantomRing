#include "Rounds.h"

#include "Combat.h"
#include "CombatLogic.h"
#include "Config.h"
#include "Explosion.h"
#include "Game.h"
#include "Log.h"
#include "Puppet.h"
#include "RenderMarkers.h"

#include "phantomring_protocol.h"

#include <algorithm>
#include <cmath>
#include <format>

#include <windows.h>

namespace phantom::er::Rounds
{
	namespace
	{
		struct Round
		{
			std::uint32_t bulletId{ 0 };
			Vec3          pos;
			Vec3          dir;
			float         speed{ 0 };
			float         travelled{ 0 };
			float         age{ 0 };
			float         radius{ 4.0f };
			Flight        kind{ Flight::kNone };
			std::string   what;
			std::vector<std::pair<Vec3, float>> trail;  // where it was, and its age then (the smoke it leaves)
		};

		std::vector<Round> g_live;
		std::uint64_t      g_launched{ 0 }, g_hitWorld{ 0 }, g_hitChr{ 0 }, g_spent{ 0 }, g_logged{ 0 };
		float              g_lastFlight{ 0 };  // s, the last round's flight time (logs)

		template <class... Args>
		void RoundLog(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (++g_logged <= 60) {
				Log::Info(a_fmt, std::forward<Args>(a_args)...);
			} else {
				Log::Debug(a_fmt, std::forward<Args>(a_args)...);
			}
		}

		float SpeedOf(Flight a_kind)
		{
			const auto& c = Cfg();
			switch (a_kind) {
			case Flight::kRocket:
				return c.roundSpeedRocket;
			case Flight::kGrenade:
				return c.roundSpeedGrenade;
			case Flight::kNone:
				break;
			}
			return 0.0f;
		}

		bool Showing(const Frame& a_f)
		{
			return Puppet::Active() && a_f.guestReady && a_f.player && !a_f.menuOpen && !a_f.cutscene && !a_f.loading && !a_f.dead && !a_f.dying &&
			       !(a_f.guest.flags & proto::kGuestInMenu);
		}

		void Color(Marker& a_m, float a_r, float a_g, float a_b, float a_a)
		{
			a_m.color[0] = a_r;
			a_m.color[1] = a_g;
			a_m.color[2] = a_b;
			a_m.color[3] = a_a;
		}

		// The round: a small dark body, a bright flame at its tail, a glowing exhaust right behind it and the grey smoke it
		// leaves along its path (fading over 0.6 s), so it reads at 50 m too.
		void Draw(const Round& a_r, std::vector<Marker>& a_out)
		{
			const bool rocket = a_r.kind == Flight::kRocket;
			Marker     body;
			body.shape = Marker::Shape::kBall;
			body.pos = a_r.pos;
			body.radius = rocket ? 0.08f : 0.05f;
			body.slack = 0.1f;
			Color(body, 0.13f, 0.13f, 0.11f, 1.0f);
			a_out.push_back(body);
			Marker flame;
			flame.shape = Marker::Shape::kDot;
			flame.pos = a_r.pos - a_r.dir * (body.radius * 1.5f);
			flame.radius = rocket ? 0.32f : 0.12f;
			flame.slack = 0.2f;
			Color(flame, 1.0f, 0.78f, 0.38f, 1.0f);
			a_out.push_back(flame);
			const float exhaust = std::min(a_r.travelled, rocket ? 1.2f : 0.5f);
			const int   n = rocket ? 6 : 3;
			for (int i = 1; i <= n; ++i) {
				const float u = static_cast<float>(i) / static_cast<float>(n);
				Marker      m;
				m.shape = Marker::Shape::kDot;
				m.pos = flame.pos - a_r.dir * (u * exhaust);
				m.radius = flame.radius * (0.8f - 0.45f * u);
				m.slack = 0.2f;
				Color(m, 1.0f, 0.55f - 0.2f * u, 0.18f, 0.95f - 0.6f * u);
				a_out.push_back(m);
			}
			if (!rocket) {
				return;
			}
			for (const auto& [p, age] : a_r.trail) {
				const float t = std::clamp((a_r.age - age) / 0.6f, 0.0f, 1.0f);
				if (t >= 1.0f || Length(p - a_r.pos) < exhaust) {
					continue;
				}
				Marker s;
				s.shape = Marker::Shape::kDot;
				s.pos = p;
				s.radius = 0.12f + 0.25f * t;
				s.slack = 0.3f;
				Color(s, 0.62f, 0.6f, 0.57f, 0.55f * (1.0f - t));
				a_out.push_back(s);
			}
		}

		void Blast(const Frame& a_f, Round& a_r, const Vec3& a_at, std::string_view a_why)
		{
			g_lastFlight = a_r.age;
			RoundLog("rounds: {} went off ({}) at ({:.2f}, {:.2f}, {:.2f}) after {:.2f} s, {:.1f} m", a_r.what, a_why, a_at.x, a_at.y, a_at.z, a_r.age, a_r.travelled);
			Combat::BlastAt(a_f, proto::kAmmoLethal, a_at - a_r.dir * 0.3f, a_r.radius, false, a_r.what);
		}
	}

	float SpeedAt(Flight a_kind, float a_age, float a_launch, float a_boost, float a_after)
	{
		if (a_kind != Flight::kRocket || !(a_boost > a_launch) || !(a_age > a_after)) {
			return a_launch;
		}
		return std::min(a_boost, a_launch + (a_boost - a_launch) * (a_age - a_after) / 0.3f);
	}

	Flight FlightOf(std::uint32_t a_id)
	{
		// TppEquip.BL_* (tools/re/data/tppequip_bullet_ids_1.0.15.4.txt).
		if ((a_id >= 43 && a_id <= 60) || a_id == 35 || a_id == 75 || a_id == 84 || a_id == 85 || a_id == 245) {
			return Flight::kRocket;  // BL_ms00..ms03 (missile launchers), BL_20mmRocket, BL_WavRocket, tank rockets, BL_HAND_KillRocket
		}
		switch (a_id) {
		case 23: case 27: case 31: case 63: case 64: case 96: case 103:
			return Flight::kGrenade;  // BL_HgGrenade, BL_40mmGrenade, BL_20mmGrenade, BL_Nad0, BL_Mortar, BL_UavGrenade, BL_EX_gl_000
		default:
			return Flight::kNone;
		}
	}

	bool Launch(const Frame& a_f, std::uint32_t a_bulletId, const Vec3& a_muzzle, const Vec3& a_dir, float a_radius, std::string a_what)
	{
		if (!Cfg().roundFlight || !Puppet::Active() || !a_f.player || !Finite(a_muzzle) || !Finite(a_dir) || Length(a_dir) < 0.5f) {
			return false;
		}
		const auto  kind = FlightOf(a_bulletId);
		const float speed = SpeedOf(kind);
		if (kind == Flight::kNone || !(speed > 0.5f)) {
			return false;
		}
		Round r;
		r.bulletId = a_bulletId;
		r.pos = a_muzzle;
		r.dir = Normalize(a_dir);
		r.speed = std::min(speed, 400.0f);
		r.radius = a_radius;
		r.kind = kind;
		r.what = std::move(a_what);
		++g_launched;
		RoundLog("rounds: {} launched at {:.0f} m/s from ({:.2f}, {:.2f}, {:.2f})", r.what, r.speed, r.pos.x, r.pos.y, r.pos.z);
		g_live.push_back(std::move(r));
		return true;
	}

	void Tick(const Frame& a_f)
	{
		std::vector<Marker> markers;
		if (!g_live.empty() && Puppet::Active() && a_f.player && game::CanCastRay()) {
			const float dt = std::clamp(a_f.dt, 0.0f, 0.1f);
			const float range = std::max(10.0f, Cfg().traceRange);
			const auto& c = Cfg();
			for (auto& r : g_live) {
				r.speed = std::min(400.0f, SpeedAt(r.kind, r.age, SpeedOf(r.kind), c.roundSpeedRocketBoost, c.roundRocketBoostAfter));
				const float step = r.speed * dt;
				const Vec3  from = r.pos;
				// The first thing on this frame's piece of the flight: Elden Ring's world, or a hostile character's capsule.
				float best = step;
				int   what = 0;  // 1 world, 2 character
				Vec3  hit;
				if (game::CastRay(from, r.dir * step, hit, a_f.player.chr, Cfg().rayFilter) && Finite(hit)) {
					best = std::min(best, Length(hit - from));
					what = 1;
				}
				const Vec3 mid = from + r.dir * (step * 0.5f);
				for (const auto& foe : Combat::HostilesNear(a_f, mid, step * 0.5f + 1.0f)) {
					if (const auto t = combat::RayCapsule(from, r.dir, best, foe.cap); t && *t < best) {
						best = *t;
						what = 2;
					}
				}
				r.pos = from + r.dir * best;
				r.travelled += best;
				r.age += dt;
				// Smoke every ~0.4 m of the flight, the last 0.6 s of it.
				if (r.trail.empty() || Length(r.trail.back().first - r.pos) >= 0.4f) {
					r.trail.emplace_back(r.pos, r.age);
				}
				std::erase_if(r.trail, [&](const auto& a_p) { return r.age - a_p.second > 0.6f; });
				if (what == 1) {
					++g_hitWorld;
					Blast(a_f, r, r.pos, "hit the world");
					r.speed = 0;
				} else if (what == 2) {
					++g_hitChr;
					Blast(a_f, r, r.pos, "hit an enemy");
					r.speed = 0;
				} else if (r.travelled >= range || r.age > 15.0f) {
					++g_spent;
					Blast(a_f, r, r.pos, "end of its range");
					r.speed = 0;
				}
			}
			std::erase_if(g_live, [](const Round& a_r) { return a_r.speed <= 0.0f; });
			if (Showing(a_f)) {
				for (const auto& r : g_live) {
					Draw(r, markers);
				}
			}
		}
		RenderMarkers::Set(RenderMarkers::Owner::kRounds, std::move(markers));
	}

	void OnPuppetStop()
	{
		if (!g_live.empty()) {
			Log::Info("rounds: puppet stopped; {} round(s) in flight dropped", g_live.size());
		}
		g_live.clear();
		RenderMarkers::Set(RenderMarkers::Owner::kRounds, {});
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		for (auto& r : g_live) {
			r.pos += a_delta;
			for (auto& p : r.trail) {
				p.first += a_delta;
			}
		}
	}

	std::string Describe()
	{
		return std::format("rounds: {} launched (rocket {:.0f} m/s, {:.0f} after {:.1f} s; grenade {:.0f} m/s{}), {} hit the world, {} an enemy, {} spent their "
		                   "range, {} in flight; last flight {:.2f} s",
			g_launched, Cfg().roundSpeedRocket, Cfg().roundSpeedRocketBoost, Cfg().roundRocketBoostAfter, Cfg().roundSpeedGrenade,
			Cfg().roundFlight ? "" : ", flight OFF", g_hitWorld, g_hitChr, g_spent, g_live.size(), g_lastFlight);
	}
}
