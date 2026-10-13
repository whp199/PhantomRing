#include "Mines.h"

#include "Combat.h"
#include "Config.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "Puppet.h"
#include "RenderMarkers.h"
#include "ThrowLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>

#include <windows.h>

namespace phantom::er::Mines
{
	namespace
	{
		struct Mine
		{
			std::uint32_t id{ 0 };
			std::uint32_t kind{ 0 };       // AmmoKind
			std::uint16_t equip{ 0 };
			float         trigger{ 0 };    // m; 0 = remote only (C4)
			Vec3          pos;             // ER world, on the ground
			Vec3          normal{ 0, 1, 0 };
			Vec3          facing{ 0, 0, 1 };
			ULONGLONG     placed{ 0 };
			ULONGLONG     detonateSent{ 0 };
			int           retries{ 0 };      // kHEvDetonate sent again after no blast came (throwing::DetonateRetryDue)
			bool          blasted{ false };
			std::string   why;             // what set it off
		};

		std::vector<Mine> g_mines;
		std::uint64_t     g_placed{ 0 }, g_detonations{ 0 }, g_blasts{ 0 }, g_gone{ 0 }, g_ticks{ 0 }, g_kept{ 0 }, g_discards{ 0 }, g_swallowed{ 0 };
		// Mines Elden Ring can't place any more (lost with a load, or announced again by a guest whose earlier host placed them):
		// MGSV is asked to set them off (kHEvDetonate; the guest skips MGSV's own blast), and their blasts are swallowed here.
		std::vector<std::uint32_t> g_discarded;
		// Kept across a stop of the puppet (an MGSV stall, Elden Ring's menu): their places are still right as long as Elden
		// Ring's world wasn't reloaded (world shifts move them, er/WorldShift.h). A load or another player object since the
		// stop: they are discarded at the next start.
		bool              g_wasActive{ false };
		bool              g_reloaded{ false };
		const void*       g_chr{ nullptr };
		std::uint32_t     g_attaches{ 0 };

		Vec3 V(const float* a_v) { return { a_v[0], a_v[1], a_v[2] }; }

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

		// Set down on Elden Ring's ground (MGSV put it on its own flat apron), with the ground's slope.
		void Ground(const Frame& a_f, Mine& a_m)
		{
			Vec3 hit;
			if (!game::CanCastRay() || !game::CastRay(a_m.pos + Vec3{ 0, 1.5f, 0 }, Vec3{ 0, -4.0f, 0 }, hit, a_f.player.chr, Cfg().rayFilter)) {
				return;
			}
			a_m.pos = hit;
			Vec3 h1, h2;
			if (game::CastRay(hit + Vec3{ 0.2f, 1.0f, 0 }, Vec3{ 0, -2.0f, 0 }, h1, a_f.player.chr, Cfg().rayFilter) &&
				game::CastRay(hit + Vec3{ 0, 1.0f, 0.2f }, Vec3{ 0, -2.0f, 0 }, h2, a_f.player.chr, Cfg().rayFilter)) {
				Vec3 n = Normalize(Cross(h2 - hit, h1 - hit));
				if (n.y < 0) {
					n = -n;
				}
				if (Finite(n) && n.y > 0.3f) {
					a_m.normal = n;
				}
			}
		}

		void Detonate(Mine& a_m, std::string a_why)
		{
			proto::HostEvent e{};
			e.type = proto::kHEvDetonate;
			e.b = a_m.id;
			e.frame = static_cast<std::uint32_t>(Host::Current().index);
			if (!Link::Get().PushHostEvent(e)) {
				Log::Warn("mines: mine {} -- host event ring full, kHEvDetonate not sent", a_m.id);
				return;
			}
			a_m.detonateSent = ::GetTickCount64();
			a_m.why = std::move(a_why);
			++g_detonations;
			Log::Info("mines: mine {} ({} equip {}) set off: {} -> kHEvDetonate", a_m.id, KindName(a_m.kind), a_m.equip, a_m.why);
		}

		void Discard(std::uint32_t a_id, std::string_view a_why)
		{
			if (std::ranges::find(g_discarded, a_id) == g_discarded.end()) {
				g_discarded.push_back(a_id);
				if (g_discarded.size() > 64) {
					g_discarded.erase(g_discarded.begin());
				}
			}
			++g_discards;
			proto::HostEvent e{};
			e.type = proto::kHEvDetonate;
			e.b = a_id;
			e.frame = static_cast<std::uint32_t>(Host::Current().index);
			const bool sent = Link::Get().PushHostEvent(e);
			Log::Info("mines: mine {} {} -- discarded{}", a_id, a_why, sent ? " (MGSV sets it off out of the way; its blast is ignored here)" : " (event ring full)");
		}

		bool Showing(const Frame& a_f)
		{
			return Puppet::Active() && a_f.guestReady && !a_f.menuOpen && !a_f.cutscene && !a_f.loading && !a_f.dead && !a_f.dying &&
			       !(a_f.guest.flags & proto::kGuestInMenu);
		}
	}

	void OnPlaced(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (!Cfg().mines || !Puppet::Active() || !a_f.player || a_e.b == 0) {
			Log::Info("mines: placed {} ignored (puppet {}, [Mines] enabled {})", a_e.b, Puppet::Active(), Cfg().mines);
			return;
		}
		const bool again = (a_e.pad[0] & proto::kPlacedAgain) != 0;
		if (auto it = std::ranges::find_if(g_mines, [&](const Mine& a_m) { return a_m.id == a_e.b; }); it != g_mines.end()) {
			// Announced again (the puppet restarted): it lies where it was put, not where MGSV's point maps through today's anchor
			// (review: re-announced mines jumped to wherever Snake was).
			it->kind = a_e.a;
			it->equip = static_cast<std::uint16_t>(a_e.pad[0] & 0xFFFF);
			it->trigger = std::clamp(static_cast<float>(a_e.pad[1]) / 1000.0f, 0.0f, 20.0f);
			++g_kept;
			Log::Info("mines: mine {} announced again -- kept where it lies, ER ({:.2f}, {:.2f}, {:.2f})", it->id, it->pos.x, it->pos.y, it->pos.z);
			return;
		}
		if (std::ranges::find(g_discarded, a_e.b) != g_discarded.end()) {
			return;  // already given back to MGSV
		}
		if (again) {
			Discard(a_e.b, "announced again but unknown here (placed before a load, or for an earlier Elden Ring session): where it lies in "
			               "Elden Ring can't be known");
			return;
		}
		const auto& anchor = Puppet::CurrentAnchor();
		Mine        m;
		m.id = a_e.b;
		m.kind = a_e.a;
		m.equip = static_cast<std::uint16_t>(a_e.pad[0] & 0xFFFF);
		m.trigger = std::clamp(static_cast<float>(a_e.pad[1]) / 1000.0f, 0.0f, 20.0f);
		m.pos = anchor.PointToEr(V(a_e.v0));
		const Vec3 f = anchor.DirToEr(V(a_e.v1));
		m.facing = LengthXZ(f) > 1e-3f ? Normalize(Vec3{ f.x, 0, f.z }) : Vec3{ 0, 0, 1 };
		m.placed = ::GetTickCount64();
		if (!Finite(m.pos)) {
			return;
		}
		Ground(a_f, m);
		std::erase_if(g_mines, [&](const Mine& a_m) { return a_m.id == m.id; });
		++g_placed;
		Log::Info("mines: mine {} placed: {} (equip {}), trigger {}, at ER ({:.2f}, {:.2f}, {:.2f})", m.id, KindName(m.kind), m.equip,
			m.trigger > 0 ? std::format("{:.1f} m", m.trigger) : std::string("remote only (C4)"), m.pos.x, m.pos.y, m.pos.z);
		g_mines.push_back(m);
	}

	void OnGone(const proto::GuestEvent& a_e)
	{
		const auto n = std::erase_if(g_mines, [&](const Mine& a_m) { return a_m.id == a_e.b; });
		if (n) {
			++g_gone;
			Log::Info("mines: mine {} gone (MGSV)", a_e.b);
		}
	}

	bool OnExplosion(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		const auto id = a_e.pad[1];
		if (id && std::ranges::find(g_discarded, id) != g_discarded.end()) {
			if (g_swallowed++ < 20) {
				Log::Info("mines: blast of discarded mine {} ignored", id);
			}
			return true;
		}
		auto it = id ? std::ranges::find_if(g_mines, [&](const Mine& a_m) { return a_m.id == id; }) : g_mines.end();
		if (it == g_mines.end()) {
			return false;
		}
		const float radius = a_e.b ? std::clamp(static_cast<float>(a_e.b) / 1000.0f, 0.5f, 30.0f) : Cfg().explosionRadius;
		++g_blasts;
		if (!it->blasted) {  // a gas mine's cloud reports again every 0.5 s: logged once
			it->blasted = true;
			Log::Info("mines: mine {} went off ({}): {} r {:.1f} m where it lies", id, it->detonateSent ? it->why : std::string("by MGSV: Snake's detonator, or its own"),
				KindName(a_e.a), radius);
		}
		Combat::BlastAt(a_f, a_e.a, it->pos + it->normal * 0.1f, radius, true, std::format("mine {} ({} equip {})", id, KindName(a_e.a), it->equip));
		// A gas mine keeps reporting its cloud with the same id; a one-shot one is gone (kGEvPlacedGone follows).
		if (a_e.a != proto::kAmmoTranq && a_e.a != proto::kAmmoSmoke) {
			g_mines.erase(it);
		}
		return true;
	}

	void Tick(const Frame& a_f)
	{
		// A new guest (MGSV restarted: its ids start again at 1): nothing kept means anything.
		if (const auto attaches = Link::Get().AttachCount(); attaches != g_attaches) {
			if (!g_mines.empty() || !g_discarded.empty()) {
				Log::Info("mines: the guest attached again; {} mine(s) forgotten", g_mines.size());
			}
			g_attaches = attaches;
			g_mines.clear();
			g_discarded.clear();
		}
		const bool active = Puppet::Active() && a_f.player;
		if (!active) {
			// While the puppet is off: a load (a death's respawn, fast travel) rebuilds Elden Ring's world around new coordinates.
			g_reloaded = g_reloaded || a_f.loading || (a_f.player && a_f.player.chr != g_chr && g_chr);
		} else {
			if (!g_wasActive && !g_mines.empty() && (g_reloaded || a_f.player.chr != g_chr)) {
				for (const auto& m : g_mines) {
					Discard(m.id, "was placed before a load (a death, fast travel): Elden Ring's world moved under it");
				}
				g_mines.clear();
			}
			g_reloaded = false;
			g_chr = a_f.player.chr;
		}
		g_wasActive = active;
		std::vector<Marker> markers;
		if (!g_mines.empty() && Puppet::Active() && a_f.player) {
			++g_ticks;
			const auto  now = ::GetTickCount64();
			const auto& c = Cfg();
			auto        armed = [&](const Mine& a_m) { return now - a_m.placed >= static_cast<ULONGLONG>(c.mineArmSeconds * 1000.0f); };
			auto        watching = [&](const Mine& a_m) { return armed(a_m) && a_m.trigger > 0 && !a_m.detonateSent; };
			// A set-off that brought no blast (MGSV refused it): sent again by the next enemy in range, a few times.
			for (auto& m : g_mines) {
				if (throwing::DetonateRetryDue(now, m.detonateSent, m.retries, m.blasted)) {
					m.detonateSent = 0;
					++m.retries;
					Log::Warn("mines: mine {}: no blast {} s after kHEvDetonate{} (the guest log says why: placed #{})", m.id, throwing::kDetonateRetryMs / 1000,
						m.retries == throwing::kDetonateRetries ? " -- one last try" : " -- it will be sent again", m.id);
				}
			}
			// Every third frame is plenty for a walking enemy. ONE scan of Elden Ring's characters around every watching mine,
			// each mine then tests that list (a scan per mine read every character's memory once per mine).
			std::vector<throwing::Sphere> watch;
			for (const auto& m : g_mines) {
				if (watching(m)) {
					watch.push_back({ m.pos + m.normal * 0.3f, m.trigger });
				}
			}
			if (!watch.empty() && (g_ticks % 3) == 0 && c.damage) {
				const auto around = throwing::Enclosing(watch);
				const auto hostiles = Combat::HostilesNear(a_f, around.centre, around.radius);
				for (auto& m : g_mines) {
					if (!watching(m)) {
						continue;
					}
					const Vec3         at = m.pos + m.normal * 0.3f;
					const Combat::Foe* nearest = nullptr;
					float              bestD = m.trigger;
					for (const auto& h : hostiles) {
						if (const float d = combat::CapsuleDistance(h.cap, at); d <= bestD) {
							bestD = d;
							nearest = &h;
						}
					}
					if (nearest) {
						Detonate(m, std::format("{} at {:.1f} m", nearest->name, bestD));
					}
				}
			}
			for (const auto& m : g_mines) {
				if (!Showing(a_f)) {
					break;
				}
				Marker body;
				body.shape = Marker::Shape::kBall;
				body.pos = m.pos + m.normal * c.mineBodyRadius;
				body.radius = c.mineBodyRadius;
				body.slack = 0.25f;  // in the grass
				const float dark[4] = { 0.06f, 0.07f, 0.05f, 1.0f };
				std::copy(std::begin(dark), std::end(dark), body.color);
				markers.push_back(body);
				// MGSV's mines blink a small light; arming and set off show it steady.
				const bool   on = m.detonateSent || !armed(m) || ((now - m.placed) % 1000) < 300;
				const float  red[4] = { 1.0f, 0.12f, 0.08f, 1.0f }, blue[4] = { 0.25f, 0.55f, 1.0f, 1.0f }, amber[4] = { 1.0f, 0.75f, 0.15f, 1.0f };
				const float* col = m.kind == proto::kAmmoTranq ? blue : m.kind == proto::kAmmoStun ? amber : red;
				if (on) {
					Marker light;
					light.shape = Marker::Shape::kDot;
					light.pos = m.pos + m.normal * (c.mineBodyRadius * 2.0f + 0.01f);
					light.radius = c.mineLightRadius;
					light.slack = 0.4f;
					std::copy(col, col + 4, light.color);
					markers.push_back(light);
				}
			}
		}
		RenderMarkers::Set(RenderMarkers::Owner::kMines, std::move(markers));
	}

	void OnPuppetStop()
	{
		// Kept (review: dropping them here made the guest's re-announcement place them next to wherever Snake started again);
		// Tick discards them at the next start if Elden Ring loaded meanwhile.
		if (!g_mines.empty()) {
			Log::Info("mines: puppet stopped; {} mine(s) kept where they lie", g_mines.size());
		}
		RenderMarkers::Set(RenderMarkers::Owner::kMines, {});
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		for (auto& m : g_mines) {
			m.pos += a_delta;
		}
	}

	std::string Describe()
	{
		return std::format("mines: {} placed, {} lying, {} set off for Elden Ring enemies, {} blasts where they lie, {} gone, {} announced again and kept, {} "
		                   "discarded (lost with a load; {} of their blasts ignored)",
			g_placed, g_mines.size(), g_detonations, g_blasts, g_gone, g_kept, g_discards, g_swallowed);
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		(void)a_f;
		if (a_w.size() > 2 && a_w[1] == "detonate") {
			const auto id = static_cast<std::uint32_t>(std::strtoul(a_w[2].c_str(), nullptr, 0));
			auto       it = std::ranges::find_if(g_mines, [&](const Mine& a_m) { return a_m.id == id; });
			if (it == g_mines.end()) {
				return std::format("no mine {}", id);
			}
			Detonate(*it, "console");
			return Describe();
		}
		std::string out = Describe();
		const auto  now = ::GetTickCount64();
		for (const auto& m : g_mines) {
			out += std::format("\n {} {} equip {} trigger {:.1f} m at ({:.2f}, {:.2f}, {:.2f}), {:.1f} s old{}", m.id, KindName(m.kind), m.equip, m.trigger, m.pos.x, m.pos.y,
				m.pos.z, static_cast<double>(now - m.placed) / 1000.0, m.detonateSent ? " -- SET OFF (" + m.why + ")" : "");
		}
		return out;
	}
}
