#include "Throw.h"

#include "Combat.h"
#include "Config.h"
#include "Game.h"
#include "Log.h"
#include "Projectiles.h"
#include "Puppet.h"
#include "RenderMarkers.h"
#include "ThrowLogic.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>

#include <windows.h>

namespace phantom::er::Throw
{
	namespace
	{
		using throwing::Projectile;
		using throwing::RayHit;

		struct Grenade
		{
			std::uint32_t id{ 0 };
			std::uint32_t kind{ 0 };      // AmmoKind of its blast
			std::uint16_t equip{ 0 };
			std::uint32_t fuseMs{ 0 };    // 0 = goes off on impact
			Projectile    p;
			bool          blasted{ false };
			ULONGLONG     blastedAt{ 0 };
			ULONGLONG     created{ 0 };
			ULONGLONG     mgsvAt{ 0 };     // MGSV reported its blast (first report)
			ULONGLONG     lastReport{ 0 };
			float         mgsvRadius{ 0 };  // m, from MGSV's report
			int           reports{ 0 };
		};

		std::vector<Grenade>                   g_live;
		std::map<std::uint16_t, float>         g_radiusByEquip;  // m, learned from MGSV's reports
		std::string                            g_arcMode;        // console override: "" = ini
		std::uint64_t                          g_throws{ 0 }, g_blasts{ 0 }, g_reportsOnCopy{ 0 }, g_lost{ 0 }, g_arcFrames{ 0 }, g_logged{ 0 };
		double                                 g_tickMs{ 0 }, g_tickMsMax{ 0 };  // cost of Tick (rays) while something is flying or aimed
		bool                                   g_aimLogged{ false };

		template <class... Args>
		void ThrowLog(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (++g_logged <= 80) {
				Log::Info(a_fmt, std::forward<Args>(a_args)...);
			} else {
				Log::Debug(a_fmt, std::forward<Args>(a_args)...);
			}
		}

		Vec3 V(const float* a_v) { return { a_v[0], a_v[1], a_v[2] }; }

		throwing::Params SimParams()
		{
			const auto&      c = Cfg();
			throwing::Params p;
			p.gravity = c.throwGravity;
			p.restitution = c.throwRestitution;
			p.friction = c.throwFriction;
			p.rollDecel = c.throwRollDecel;
			return p;
		}

		// Elden Ring's world as the simulation sees it: cs_phys_world_cast_ray (the puppet ignored), and the surface's
		// normal from two parallel rays beside the hit (the cast returns only the point).
		throwing::RayFn GameRay(const Frame& a_f)
		{
			return [chr = a_f.player.chr](const Vec3& a_from, const Vec3& a_delta) -> std::optional<RayHit> {
				Vec3 hit;
				if (!game::CastRay(a_from, a_delta, hit, chr, Cfg().rayFilter)) {
					return std::nullopt;
				}
				const Vec3 d = Normalize(a_delta);
				const Vec3 u = std::abs(d.y) < 0.9f ? Normalize(Cross(d, { 0, 1, 0 })) : Normalize(Cross(d, { 1, 0, 0 }));
				const Vec3 w = Cross(d, u);
				const Vec3 fallback = d.y < -0.3f ? Vec3{ 0, 1, 0 } : -d;
				Vec3       h1, h2;
				const Vec3 back = hit - d * 0.4f;
				if (game::CastRay(back + u * 0.15f, d * 0.8f, h1, chr, Cfg().rayFilter) && game::CastRay(back + w * 0.15f, d * 0.8f, h2, chr, Cfg().rayFilter)) {
					return RayHit{ hit, throwing::PlaneNormal(hit, h1, h2, -d, fallback) };
				}
				return RayHit{ hit, fallback };
			};
		}

		float DefaultRadius(std::uint32_t a_kind)
		{
			const auto& c = Cfg();
			switch (a_kind) {
			case proto::kAmmoTranq:
				return c.throwRadiusSleep;
			case proto::kAmmoStun:
				return c.throwRadiusStun;
			case proto::kAmmoSmoke:
				return c.throwRadiusSmoke;
			default:
				return c.throwRadiusLethal;
			}
		}

		const char* KindName(std::uint32_t a_kind)
		{
			switch (a_kind) {
			case proto::kAmmoTranq:
				return "sleep";
			case proto::kAmmoStun:
				return "stun";
			case proto::kAmmoSmoke:
				return "smoke";
			default:
				return "lethal";
			}
		}

		bool IsCloud(std::uint32_t a_kind) { return a_kind == proto::kAmmoTranq || a_kind == proto::kAmmoSmoke; }

		void Blast(const Frame& a_f, Grenade& a_g, float a_radius, std::string_view a_why)
		{
			const Vec3 at = a_g.p.pos;
			Combat::BlastAt(a_f, a_g.kind, at, a_radius, a_g.p.touched, std::format("throw {} ({} {}, {})", a_g.id, KindName(a_g.kind), a_g.equip, a_why));
			if (!a_g.blasted) {
				a_g.blasted = true;
				a_g.blastedAt = ::GetTickCount64();
				++g_blasts;
				ThrowLog("throw {}: {} blast r {:.1f} m at ({:.2f}, {:.2f}, {:.2f}) -- {} ({:.2f} s after the release, {} bounce(s){})", a_g.id, KindName(a_g.kind),
					a_radius, at.x, at.y, at.z, a_why, a_g.p.age, a_g.p.bounces, a_g.p.resting ? ", lying still" : a_g.p.touched ? ", rolling" : ", in the air");
			}
		}

		bool Showing(const Frame& a_f)
		{
			return Puppet::Active() && a_f.guestReady && a_f.player && !a_f.menuOpen && !a_f.cutscene && !a_f.loading && !a_f.dead && !a_f.dying &&
			       !(a_f.guest.flags & proto::kGuestInMenu);
		}

		bool ArcWanted()
		{
			const std::string_view mode = g_arcMode.empty() ? std::string_view(Cfg().throwArc) : std::string_view(g_arcMode);
			return mode != "off";
		}

		void PushColor(Marker& a_m, const float (&a_c)[4])
		{
			for (int i = 0; i < 4; ++i) {
				a_m.color[i] = a_c[i];
			}
		}

		// MGSV's aiming arc: a thin line of bright dots and a ring where it lands.
		void Arc(const Frame& a_f, std::vector<Marker>& a_out)
		{
			const auto& g = a_f.guest;
			const bool  forced = g_arcMode == "on";
			if (!(g.flags & proto::kGuestThrowAim) && !forced) {
				return;
			}
			const auto& anchor = Puppet::CurrentAnchor();
			Vec3        pos = anchor.PointToEr(V(g.aimThrowPos));
			Vec3        vel = anchor.DirToEr(V(g.aimThrowVel));
			if (forced && !(g.flags & proto::kGuestThrowAim)) {
				// Console test without MGSV aiming: from Snake's chest along the camera, 12 m/s.
				const auto* cam = game::PersCam1();
				Vec3        fwd = cam ? Normalize(Vec3{ reinterpret_cast<const float*>(cam + game::kCamMatrix + 0x20)[0],
									  reinterpret_cast<const float*>(cam + game::kCamMatrix + 0x20)[1], reinterpret_cast<const float*>(cam + game::kCamMatrix + 0x20)[2] })
				                      : Vec3{ 0, 0, 1 };
				pos = anchor.pe + Vec3{ 0, 1.5f, 0 };
				vel = Normalize(fwd + Vec3{ 0, 0.35f, 0 }) * 12.0f;
			}
			if (!Finite(pos) || !Finite(vel) || Length(vel) < 0.5f || Length(vel) > 80.0f) {
				return;
			}
			if (!g_aimLogged) {
				g_aimLogged = true;
				Log::Info("throw: aiming arc from ({:.2f}, {:.2f}, {:.2f}) at {:.1f} m/s (kind {})", pos.x, pos.y, pos.z, Length(vel), g.aimThrowKind);
			}
			auto params = SimParams();
			params.maxStep = 1.0f / 30.0f;  // the arc's chords: ~1 ray per 2 frames of flight, the dots stay on the parabola
			const auto  arc = throwing::TraceArc(pos, vel, params, GameRay(a_f), std::max(0.1f, Cfg().throwArcSpacing), 4.0f);
			const auto& c = Cfg();
			// The first dots sit in Snake's hand: start a little out.
			for (std::size_t i = 2; i < arc.dots.size(); ++i) {
				Marker m;
				m.shape = Marker::Shape::kDot;
				m.pos = arc.dots[i];
				m.radius = c.throwArcDot;
				PushColor(m, c.throwArcColor);
				a_out.push_back(m);
			}
			if (arc.landing) {
				Marker ring;
				ring.shape = Marker::Shape::kRing;
				ring.pos = arc.landing->point;
				ring.normal = arc.landing->normal;
				ring.radius = c.throwArcRing;
				ring.inner = 0.72f;
				ring.slack = 0.35f;
				PushColor(ring, c.throwArcColor);
				a_out.push_back(ring);
				Marker centre = ring;
				centre.shape = Marker::Shape::kDot;
				centre.pos = arc.landing->point + arc.landing->normal * 0.05f;
				centre.radius = c.throwArcDot * 1.6f;
				centre.slack = 0.35f;
				a_out.push_back(centre);
			}
			++g_arcFrames;
		}
	}

	void OnThrow(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (!Puppet::Active() || !a_f.player || !Cfg().throwSim) {
			ThrowLog("throw {} ignored (puppet {}, [Throw] sim {})", a_e.b, Puppet::Active(), Cfg().throwSim);
			return;
		}
		const auto& anchor = Puppet::CurrentAnchor();
		Grenade     g;
		g.id = a_e.b;
		g.kind = a_e.a;
		g.equip = throwing::EquipOf(a_e.pad[0]);
		g.fuseMs = throwing::FuseMsOf(a_e.pad[0]);
		g.p.pos = anchor.PointToEr(V(a_e.v0));
		g.p.vel = anchor.DirToEr(V(a_e.v1));
		g.created = ::GetTickCount64();
		if (!Finite(g.p.pos) || !Finite(g.p.vel) || Length(g.p.vel) > 100.0f || g.id == 0) {
			ThrowLog("throw {}: bad release ({}, speed {:.1f})", g.id, Finite(g.p.pos) ? "ok" : "no position", Length(g.p.vel));
			return;
		}
		std::erase_if(g_live, [&](const Grenade& a_g) { return a_g.id == g.id; });
		++g_throws;
		ThrowLog("throw {}: {} (equip {}, {}) released at ER ({:.2f}, {:.2f}, {:.2f}) at {:.1f} m/s", g.id, KindName(g.kind), g.equip,
			g.fuseMs ? std::format("fuse {} ms", g.fuseMs) : std::string("impact"), g.p.pos.x, g.p.pos.y, g.p.pos.z, Length(g.p.vel));
		g_live.push_back(g);
		Projectiles::NoteRelease(Projectiles::KeyOf(Projectiles::Source::kThrow, g.equip), g.p.vel);  // MGSV's mesh of it (roadmap 5.4)
	}

	bool OnExplosion(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		const std::uint32_t id = a_e.pad[1];
		if (id == 0) {
			return false;
		}
		auto it = std::ranges::find_if(g_live, [&](const Grenade& a_g) { return a_g.id == id; });
		if (it == g_live.end()) {
			return false;
		}
		auto&       g = *it;
		const auto  now = ::GetTickCount64();
		const float radius = a_e.b ? std::clamp(static_cast<float>(a_e.b) / 1000.0f, 0.5f, 30.0f) : DefaultRadius(g.kind);
		g.kind = a_e.a;  // MGSV's blast decides (a stun grenade's flash, a gas grenade's cloud)
		g.mgsvRadius = radius;
		g.lastReport = now;
		++g.reports;
		++g_reportsOnCopy;
		if (!g.mgsvAt) {
			g.mgsvAt = now;
			g_radiusByEquip[g.equip] = radius;
		}
		if (g.p.lost) {
			return false;  // our copy fell out of the world: MGSV's own point (through the anchor) it is
		}
		if (!g.blasted) {
			// An impact grenade our copy hasn't landed yet: it goes off when it does (Tick), at most impact_wait later.
			if (g.fuseMs == 0 && !g.p.touched) {
				return true;
			}
			Blast(a_f, g, radius, g.fuseMs ? "MGSV's fuse" : "MGSV's impact");
			return true;
		}
		if (IsCloud(g.kind)) {
			Blast(a_f, g, radius, "the cloud goes on");  // the gas / smoke MGSV reports every 0.5 s, on our copy
		}
		return true;  // a one-shot blast already went off where our copy landed
	}

	void Tick(const Frame& a_f)
	{
		LARGE_INTEGER t0, t1, freq;
		::QueryPerformanceCounter(&t0);
		std::vector<Marker> markers;
		std::vector<Projectiles::Flying> flying;  // drawn with MGSV's mesh when one was learned (er/Projectiles.h)
		const bool          showing = Showing(a_f);
		if (!g_live.empty() && Puppet::Active() && a_f.player && game::CanCastRay()) {
			const auto ray = GameRay(a_f);
			const auto params = SimParams();
			const auto now = ::GetTickCount64();
			const auto& c = Cfg();
			for (auto& g : g_live) {
				const bool wasTouched = g.p.touched;
				throwing::Step(g.p, a_f.dt, params, ray, g.fuseMs == 0);
				if (g.p.lost && !g.blasted) {
					++g_lost;
					ThrowLog("throw {}: our copy is lost ({:.1f} s, at ({:.1f}, {:.1f}, {:.1f})); MGSV's own point will be used", g.id, g.p.age, g.p.pos.x, g.p.pos.y,
						g.p.pos.z);
				}
				if (!g.blasted && !g.p.lost && g.fuseMs == 0) {
					const bool landedNow = g.p.touched && !wasTouched;
					const bool waitedEnough = g.mgsvAt && now - g.mgsvAt > static_cast<ULONGLONG>(c.throwImpactWait * 1000.0f);
					if (landedNow || waitedEnough || (g.p.touched && g.mgsvAt)) {
						const auto known = g_radiusByEquip.find(g.equip);
						const float radius = g.mgsvRadius > 0 ? g.mgsvRadius : known != g_radiusByEquip.end() ? known->second : DefaultRadius(g.kind);
						Blast(a_f, g, radius, landedNow ? "landed (impact)" : "MGSV went off first");
					}
				}
				const auto key = Projectiles::KeyOf(Projectiles::Source::kThrow, g.equip);
				if (showing && !g.p.lost && !(g.blasted && !IsCloud(g.kind))) {
					flying.push_back({ key, g.id, g.p.pos, g.p.vel, g.p.age });
				}
				// The ball only goes once MGSV's mesh was really put at this copy (last frame's scene), not merely when one is known.
				if (showing && !g.p.lost && !(g.blasted && !IsCloud(g.kind)) && !Projectiles::Drawn(Projectiles::Source::kThrow, g.id, a_f.index)) {
					Marker m;
					m.shape = Marker::Shape::kBall;
					m.pos = g.p.pos;
					m.radius = c.throwBallRadius;
					m.slack = 0.1f;
					const float olive[4] = { 0.10f, 0.13f, 0.07f, 1.0f };
					PushColor(m, olive);
					markers.push_back(m);
				}
			}
			// Done: a one-shot blast went off; a cloud MGSV stopped reporting 3 s ago; nothing from MGSV for 20 s.
			std::erase_if(g_live, [&](const Grenade& a_g) {
				if (a_g.blasted && !IsCloud(a_g.kind)) {
					// Kept until MGSV's own report of it is in (it must not go off a second time through the anchor).
					return a_g.reports > 0 || now - a_g.blastedAt > 8000;
				}
				if (a_g.blasted) {
					return now - std::max(a_g.lastReport, a_g.created) > 3000;
				}
				return now - a_g.created > 20000 || (a_g.p.lost && now - a_g.created > 5000);
			});
		}
		if (showing && Cfg().throwSim && ArcWanted() && game::CanCastRay()) {
			Arc(a_f, markers);
		}
		if (!markers.empty()) {
			::QueryPerformanceCounter(&t1);
			::QueryPerformanceFrequency(&freq);
			const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
			g_tickMs = g_tickMs * 0.95 + ms * 0.05;
			g_tickMsMax = std::max(g_tickMsMax, ms);
		}
		RenderMarkers::Set(RenderMarkers::Owner::kThrow, std::move(markers));
		Projectiles::Set(Projectiles::Source::kThrow, std::move(flying));
	}

	void OnPuppetStop()
	{
		if (!g_live.empty()) {
			Log::Info("throw: puppet stopped; {} grenade(s) no longer followed", g_live.size());
		}
		g_live.clear();
		RenderMarkers::Set(RenderMarkers::Owner::kThrow, {});
		Projectiles::Set(Projectiles::Source::kThrow, {});
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		for (auto& g : g_live) {
			g.p.pos += a_delta;
		}
	}

	std::string Describe()
	{
		return std::format("throws: {} received, {} blasts on our copy, {} MGSV reports matched, {} copies lost, {} in flight/lying, arc {} ({} frames drawn), "
		                   "{} equip radii learned; {:.3f} ms per frame flying/aiming (max {:.3f})",
			g_throws, g_blasts, g_reportsOnCopy, g_lost, g_live.size(), g_arcMode.empty() ? Cfg().throwArc : g_arcMode, g_arcFrames, g_radiusByEquip.size(), g_tickMs,
			g_tickMsMax);
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		(void)a_f;
		if (a_w.size() > 2 && a_w[1] == "meshes") {
			// throw meshes on|off: [Render] projectile_meshes live (not saved; the guest's [Capture] ProjectileMeshes tags the draws)
			if (a_w[2] != "on" && a_w[2] != "off") {
				return "throw meshes on|off";
			}
			Cfg().render.projectileMeshes = a_w[2] == "on";
			return Describe() + "\n" + Projectiles::Describe();
		}
		if (a_w.size() > 2 && a_w[1] == "arc") {
			if (a_w[2] != "on" && a_w[2] != "off" && a_w[2] != "auto" && a_w[2] != "ini") {
				return "throw arc on|off|auto|ini";
			}
			g_arcMode = a_w[2] == "ini" ? "" : a_w[2];
			return Describe();
		}
		std::string out = Describe() + "\n" + Projectiles::Describe();
		for (const auto& g : g_live) {
			out += std::format("\n {} {} equip {} {}: ({:.2f}, {:.2f}, {:.2f}) v {:.1f} m/s, {:.1f} s, {} bounce(s){}{}{}", g.id, KindName(g.kind), g.equip,
				g.fuseMs ? std::format("fuse {} ms", g.fuseMs) : "impact", g.p.pos.x, g.p.pos.y, g.p.pos.z, Length(g.p.vel), g.p.age, g.p.bounces,
				g.p.resting ? " resting" : g.p.rolling ? " rolling" : g.p.touched ? " bounced" : " flying", g.blasted ? " BLASTED" : "", g.p.lost ? " LOST" : "");
		}
		return out;
	}
}
