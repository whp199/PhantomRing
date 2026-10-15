#include "Decoys.h"

#include "Addresses.h"
#include "Anchor.h"
#include "Calibration.h"
#include "Combat.h"
#include "Config.h"
#include "DecoyLogic.h"
#include "Game.h"
#include "Link.h"
#include "Log.h"
#include "Mem.h"
#include "Puppet.h"
#include "RenderMarkers.h"
#include "ThrowLogic.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <format>
#include <map>

#include <windows.h>

namespace phantom::er::Decoys
{
	namespace
	{
		using SetYawFn = void (*)(void* a_chr, const float* a_euler);
		constexpr std::uint8_t  kStareBits = 0x30;           // ChrIns+0x538: bit4 no attack, bit5 no move (as er/Stealth.cpp's poster)
		constexpr std::uint32_t kConsoleIds = 0x80000000u;   // `decoys add`: never sent to the guest
		constexpr ULONGLONG     kScanMs = 250;               // enemies looked at 4 times a second, as the poster

		struct Decoy
		{
			std::uint32_t  id{ 0 };
			std::uint16_t  equip{ 0 };
			decoy::Variant variant{ decoy::Variant::kPlain };
			Vec3           pos;            // ER world, its feet on the ground
			Vec3           facing{ 0, 0, 1 };
			ULONGLONG      placed{ 0 };
			ULONGLONG      popped{ 0 };    // 0 = standing; else when it popped (drawn collapsing, then dropped)
			std::uint32_t  stares{ 0 };
			std::string    why;            // what popped it
		};
		struct Starer
		{
			std::uint8_t* chr{ nullptr };
			std::uint64_t handle{ 0 };
			std::uint8_t  setBits{ 0 };    // the bits we set (that were clear): only those go back
			ULONGLONG     until{ 0 };
			ULONGLONG     lastTurn{ 0 };
			std::uint32_t decoy{ 0 };
			std::string   name;
		};

		std::vector<Decoy>                 g_decoys;
		std::vector<Starer>                g_starers;
		std::map<std::uint64_t, ULONGLONG> g_cooldown;  // handle -> no stare before
		std::uint64_t g_placed{ 0 }, g_gone{ 0 }, g_pops{ 0 }, g_stares{ 0 }, g_heard{ 0 }, g_shocks{ 0 }, g_turns{ 0 }, g_ignored{ 0 }, g_dropped{ 0 };
		ULONGLONG     g_lastScan{ 0 };
		std::uint32_t g_consoleNext{ kConsoleIds };
		// As er/Mines.cpp: kept across a stop of the puppet unless Elden Ring loaded (or swapped the player object) meanwhile.
		bool          g_wasActive{ false };
		bool          g_reloaded{ false };
		const void*   g_chr{ nullptr };
		std::uint32_t g_attaches{ 0 };

		Vec3 V(const float* a_v) { return { a_v[0], a_v[1], a_v[2] }; }

		decoy::Rules Rules()
		{
			const auto&  c = Cfg();
			decoy::Rules r;
			r.range = c.decoyRange;
			r.activeRange = c.decoyActiveRange;
			r.coneDeg = c.decoyCone;
			r.popDistance = c.decoyPopDistance;
			r.snakeClose = c.decoySnakeClose;
			return r;
		}

		std::int64_t PoppedAge(const Decoy& a_d, ULONGLONG a_now) { return a_d.popped ? static_cast<std::int64_t>(a_now - a_d.popped) : -1; }

		// Set down on Elden Ring's ground (MGSV set it on its own flat apron).
		void Ground(const Frame& a_f, Decoy& a_d)
		{
			if (!game::CanCastRay()) {
				return;
			}
			// MGSV throws a decoy (LT, RT) and it lands on its flat apron, i.e. at Snake's own height: Elden Ring's ground there
			// can be far below it (thrown down a slope: live pass B 2026-10-15, one stood 5.5 m up in the air) or above it
			// (thrown uphill). Near that height first, then down to 40 m below, then from 12 m above.
			struct Try
			{
				float from, down;
			};
			for (const Try t : { Try{ 1.5f, 4.0f }, Try{ 1.5f, 40.0f }, Try{ 12.0f, 10.5f } }) {
				Vec3 hit;
				if (game::CastRay(a_d.pos + Vec3{ 0, t.from, 0 }, Vec3{ 0, -t.down, 0 }, hit, a_f.player.chr, Cfg().rayFilter)) {
					if (std::abs(hit.y - a_d.pos.y) > 2.5f) {
						Log::Info("decoys: decoy {} set down on Elden Ring's ground {:+.1f} m from where MGSV's apron put it", a_d.id, hit.y - a_d.pos.y);
					}
					a_d.pos = hit;
					return;
				}
			}
		}

		bool Showing(const Frame& a_f)
		{
			return Puppet::Active() && a_f.guestReady && !a_f.menuOpen && !a_f.cutscene && !a_f.loading && !a_f.dead && !a_f.dying &&
			       !(a_f.guest.flags & proto::kGuestInMenu);
		}

		void Release(const Starer& a_s, const std::vector<game::Character>& a_chars)
		{
			for (const auto& c : a_chars) {
				if (c.handle == a_s.handle && c.chr == a_s.chr) {
					auto*      flags = a_s.chr + game::kChrDebugFlags;
					const auto v = SafeRead<std::uint8_t>(flags).value_or(0);
					SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(v & ~a_s.setBits));
					return;
				}
			}
		}

		// a_which: 0 = every starer, else only those staring at that decoy.
		void ReleaseStarers(std::uint32_t a_which, const char* a_why)
		{
			if (g_starers.empty()) {
				return;
			}
			const auto  chars = game::Characters(256);
			std::size_t n = 0;
			std::erase_if(g_starers, [&](const Starer& s) {
				if (a_which && s.decoy != a_which) {
					return false;
				}
				Release(s, chars);
				++n;
				return true;
			});
			if (n) {
				Log::Info("decoys: {} starer(s) go on ({})", n, a_why);
			}
		}

		// The enemy turns to look at it (only with [Decoys] turn: SetYaw is verified on the player only).
		void Turn(std::uint8_t* a_chr, const Vec3& a_from, const Vec3& a_to)
		{
			const Vec3 flat{ a_to.x - a_from.x, 0, a_to.z - a_from.z };
			if (!Cfg().decoyTurn || !Addr().Has(A::ChrInsSetYaw) || LengthXZ(flat) < 0.2f) {
				return;
			}
			Axes axes;
			axes.erForward = static_cast<float>(Calibration::ErForward());
			alignas(16) float euler[4] = { 0, ErYawForHeading(Heading(flat), axes), 0, 0 };  // the yaw is the second angle (er/Game.cpp SetYaw)
			Addr().Fn<SetYawFn>(A::ChrInsSetYaw)(a_chr, euler);
			++g_turns;
		}

		// kHEvDetonate for a guest's decoy: to the guest that is a pop -- it sends kGEvPlacedGone and never announces it again.
		// Also how the host lets go of one it drops without a pop (set before a load; re-announced but unknown here): as the
		// mines' Discard, else the guest announced it again at every start of the puppet (review).
		bool TellGuestGone(const Frame& a_f, std::uint32_t a_id)
		{
			if (a_id == 0 || a_id >= kConsoleIds) {
				return false;
			}
			proto::HostEvent e{};
			e.type = proto::kHEvDetonate;
			e.b = a_id;
			e.frame = static_cast<std::uint32_t>(a_f.index);
			return Link::Get().PushHostEvent(e);
		}

		void Pop(const Frame& a_f, Decoy& a_d, std::string a_why)
		{
			if (a_d.popped) {
				return;
			}
			a_d.popped = ::GetTickCount64();
			a_d.why = std::move(a_why);
			++g_pops;
			ReleaseStarers(a_d.id, "their decoy popped");
			const bool sent = TellGuestGone(a_f, a_d.id);
			Log::Info("decoys: {} {} popped: {}{}", decoy::VariantName(a_d.variant), a_d.id, a_d.why,
				a_d.id >= kConsoleIds ? " (console decoy)" : sent ? " -> kHEvDetonate" : " (host event ring full: the guest keeps it)");
			// MGSV's shock decoy shocks whoever knocks it over: a stun blast of Elden Ring's (stagger, er/Combat.h).
			if (a_d.variant == decoy::Variant::kShock && Cfg().decoyShockRadius > 0.0f && Cfg().damage) {
				++g_shocks;
				Combat::BlastAt(a_f, proto::kAmmoStun, decoy::Chest(a_d.pos, Cfg().decoyHeight), Cfg().decoyShockRadius, false,
					std::format("shock decoy {}", a_d.id));
			}
		}

		void Add(const Frame& a_f, Decoy a_d)
		{
			Ground(a_f, a_d);
			std::erase_if(g_decoys, [&](const Decoy& d) { return d.id == a_d.id; });
			++g_placed;
			Log::Info("decoys: {} {} set (equip {}) at ER ({:.2f}, {:.2f}, {:.2f})", decoy::VariantName(a_d.variant), a_d.id, a_d.equip, a_d.pos.x, a_d.pos.y,
				a_d.pos.z);
			g_decoys.push_back(std::move(a_d));
		}

		// Enemies near standing decoys: a close one knocks it over, one that notices it stares.
		void Scan(const Frame& a_f, ULONGLONG a_now)
		{
			const auto& c = Cfg();
			const auto  rules = Rules();
			std::vector<throwing::Sphere> around;
			for (const auto& d : g_decoys) {
				if (decoy::Standing(a_now - d.placed, PoppedAge(d, a_now), rules)) {
					around.push_back({ decoy::Chest(d.pos, c.decoyHeight), std::max({ rules.range, rules.activeRange, rules.popDistance }) });
				}
			}
			if (around.empty()) {
				return;
			}
			const auto  all = throwing::Enclosing(around);
			const Vec3  snake = game::Position(a_f.player) + Vec3{ 0, 1.0f, 0 };
			const float stareS = std::max(0.5f, c.decoyStare);
			for (const auto& foe : Combat::HostilesNear(a_f, all.centre, all.radius)) {
				if (Combat::Asleep(foe.chr)) {
					continue;  // a sleeper sees nothing (and won't knock anything over)
				}
				const float toSnake = combat::CapsuleDistance(foe.cap, snake);
				// Walked into one: it pops (staring or not: a starer doesn't move, so it was there already -- it knocks it too).
				Decoy*      best = nullptr;
				float       bestD = 1e9f;
				decoy::Notice how = decoy::Notice::kNo;
				for (auto& d : g_decoys) {
					if (!decoy::Standing(a_now - d.placed, PoppedAge(d, a_now), rules)) {
						continue;
					}
					const Vec3  chest = decoy::Chest(d.pos, c.decoyHeight);
					if (decoy::PopsOn(combat::CapsuleDistance(foe.cap, chest), rules)) {
						Pop(a_f, d, std::format("{} walked into it", foe.name));
						continue;
					}
					const Vec3  eye = foe.cap.b;  // top sphere centre: about the eyes
					const Vec3  fwd = Rotate(foe.orientation, { 0, 0, 1 }) * static_cast<float>(Calibration::ErForward());
					const auto  n = decoy::Notices(eye, fwd, chest, d.variant, toSnake, rules);
					const float dist = Length(chest - eye);
					if (n == decoy::Notice::kNo || dist >= bestD) {
						continue;
					}
					Vec3 hit;
					if (n == decoy::Notice::kSees && game::CastRay(eye, (chest - eye) * 0.97f, hit, a_f.player.chr, c.rayFilter)) {
						continue;  // a wall in between
					}
					best = &d;
					bestD = dist;
					how = n;
				}
				if (!best || std::ranges::any_of(g_starers, [&](const Starer& s) { return s.handle == foe.handle; })) {
					continue;
				}
				if (auto it = g_cooldown.find(foe.handle); it != g_cooldown.end() && a_now < it->second) {
					continue;
				}
				auto*      flags = foe.chr + game::kChrDebugFlags;
				const auto v = SafeRead<std::uint8_t>(flags);
				if (!v) {
					continue;
				}
				Starer s;
				s.chr = foe.chr;
				s.handle = foe.handle;
				s.setBits = static_cast<std::uint8_t>(kStareBits & ~*v);
				s.until = a_now + static_cast<ULONGLONG>(stareS * 1000.0f);
				s.decoy = best->id;
				s.name = foe.name;
				SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(*v | kStareBits));
				Turn(foe.chr, foe.cap.a, best->pos);
				s.lastTurn = a_now;
				g_starers.push_back(s);
				g_cooldown[foe.handle] = s.until + static_cast<ULONGLONG>(std::max(0.0f, c.decoyCooldown) * 1000.0f);
				++best->stares;
				++(how == decoy::Notice::kHears ? g_heard : g_stares);
				Log::Info("decoys: {} {} the {} {} {:.1f} m away and stops to stare ({:.0f} s)", foe.name, how == decoy::Notice::kHears ? "hears" : "sees",
					decoy::VariantName(best->variant), best->id, bestD, stareS);
			}
		}

		void Draw(const Frame& a_f, ULONGLONG a_now)
		{
			std::vector<Marker> markers;
			if (!Showing(a_f)) {
				RenderMarkers::Set(RenderMarkers::Owner::kDecoys, std::move(markers));
				return;
			}
			const auto  rules = Rules();
			const float h = Cfg().decoyHeight;
			for (const auto& d : g_decoys) {
				const float scale = decoy::Scale(a_now - d.placed, PoppedAge(d, a_now), rules);
				// MGSV's decoy: a grey-olive rubber soldier; the shock decoy a little bluer.
				const float olive[4] = { 0.20f, 0.22f, 0.15f, 1.0f }, steel[4] = { 0.16f, 0.19f, 0.22f, 1.0f };
				const float* col = d.variant == decoy::Variant::kShock ? steel : olive;
				for (const auto& b : decoy::Figure(d.pos, d.facing, h, scale)) {
					Marker m;
					m.shape = Marker::Shape::kBall;
					m.pos = b.pos;
					m.radius = b.radius;
					m.slack = 0.1f;
					std::copy(col, col + 4, m.color);
					markers.push_back(m);
				}
				// The active decoy's speaker blinks while it calls out; the shock decoy's charge shows steady.
				const bool standing = decoy::Standing(a_now - d.placed, PoppedAge(d, a_now), rules);
				if (standing && d.variant != decoy::Variant::kPlain && (d.variant == decoy::Variant::kShock || ((a_now - d.placed) % 900) < 450)) {
					Marker light;
					light.shape = Marker::Shape::kDot;
					light.pos = decoy::Chest(d.pos, h) + d.facing * (0.13f * h);
					light.radius = 0.03f;
					light.slack = 0.2f;
					const float amber[4] = { 1.0f, 0.7f, 0.15f, 1.0f }, blue[4] = { 0.35f, 0.65f, 1.0f, 1.0f };
					std::copy_n(d.variant == decoy::Variant::kShock ? blue : amber, 4, light.color);
					markers.push_back(light);
				}
			}
			RenderMarkers::Set(RenderMarkers::Owner::kDecoys, std::move(markers));
		}
	}

	void OnPlaced(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (!Cfg().decoys || !Puppet::Active() || !a_f.player || a_e.b == 0 || a_e.b >= kConsoleIds) {
			Log::Info("decoys: decoy {} ignored (puppet {}, [Decoys] enabled {})", a_e.b, Puppet::Active(), Cfg().decoys);
			return;
		}
		const auto equip = static_cast<std::uint16_t>(a_e.pad[0] & 0xFFFF);
		if (auto it = std::ranges::find_if(g_decoys, [&](const Decoy& d) { return d.id == a_e.b; }); it != g_decoys.end()) {
			Log::Info("decoys: decoy {} announced again -- kept where it stands", a_e.b);
			return;
		}
		if (a_e.pad[0] & proto::kPlacedAgain) {
			// Set before a load or for an earlier Elden Ring session: where it stands in Elden Ring can't be known (it stays in MGSV).
			// The guest is told it is gone for us (as a pop), so it isn't announced again at the next start.
			++g_ignored;
			const bool told = TellGuestGone(a_f, a_e.b);
			Log::Info("decoys: decoy {} announced again but unknown here -- not shown{}", a_e.b, told ? ", the guest drops it (kHEvDetonate)" : " (host event ring full)");
			return;
		}
		Decoy d;
		d.id = a_e.b;
		d.equip = equip;
		d.variant = decoy::VariantOf(equip);
		const auto& anchor = Puppet::CurrentAnchor();
		d.pos = anchor.PointToEr(V(a_e.v0));
		const Vec3 f = anchor.DirToEr(V(a_e.v1));
		d.facing = LengthXZ(f) > 1e-3f ? Normalize(Vec3{ f.x, 0, f.z }) : Vec3{ 0, 0, 1 };
		d.placed = ::GetTickCount64();
		if (!Finite(d.pos)) {
			return;
		}
		Add(a_f, std::move(d));
	}

	void OnGone(const proto::GuestEvent& a_e)
	{
		auto it = std::ranges::find_if(g_decoys, [&](const Decoy& d) { return d.id == a_e.b; });
		if (it == g_decoys.end()) {
			return;
		}
		++g_gone;
		// A pop's own kGEvPlacedGone comes back while it collapses: let it finish.
		if (!it->popped) {
			Log::Info("decoys: decoy {} gone (MGSV: picked up)", a_e.b);
			ReleaseStarers(a_e.b, "their decoy is gone");
			g_decoys.erase(it);
		}
	}

	void Tick(const Frame& a_f)
	{
		// A new guest (MGSV restarted: its ids start again at 1): nothing kept means anything.
		if (const auto attaches = Link::Get().AttachCount(); attaches != g_attaches) {
			g_attaches = attaches;
			if (!g_decoys.empty()) {
				Log::Info("decoys: the guest attached again; {} decoy(s) forgotten", g_decoys.size());
				ReleaseStarers(0, "the guest attached again");
				g_decoys.clear();
			}
		}
		const bool active = Puppet::Active() && a_f.player;
		if (!active) {
			g_reloaded = g_reloaded || a_f.loading || (a_f.player && a_f.player.chr != g_chr && g_chr);
		} else {
			if (!g_wasActive && !g_decoys.empty() && (g_reloaded || a_f.player.chr != g_chr)) {
				g_dropped += g_decoys.size();
				int told = 0;
				for (const auto& d : g_decoys) {
					told += !d.popped && TellGuestGone(a_f, d.id) ? 1 : 0;  // the guest drops them too (not announced at every start)
				}
				Log::Info("decoys: {} decoy(s) set before a load (a death, fast travel) dropped: Elden Ring's world moved under them ({} told to the guest)",
					g_decoys.size(), told);
				g_decoys.clear();
			}
			g_reloaded = false;
			g_chr = a_f.player.chr;
		}
		g_wasActive = active;
		const auto now = ::GetTickCount64();
		const auto rules = Rules();
		std::erase_if(g_decoys, [&](const Decoy& d) { return d.popped && now - d.popped > rules.popMs; });
		const bool run = active && Cfg().decoys && a_f.guestReady && !a_f.dead && !a_f.dying && !a_f.loading;
		if (!run || g_decoys.empty()) {
			ReleaseStarers(0, !run ? "Snake is not playing" : "no decoy left");
			RenderMarkers::Set(RenderMarkers::Owner::kDecoys, {});
			std::erase_if(g_cooldown, [&](const auto& a_kv) { return now > a_kv.second; });
			return;
		}
		if (now - g_lastScan >= kScanMs) {
			g_lastScan = now;
			Scan(a_f, now);
		}
		if (!g_starers.empty()) {
			const auto chars = game::Characters(256);
			std::erase_if(g_starers, [&](Starer& s) {
				auto it = std::ranges::find_if(g_decoys, [&](const Decoy& d) { return d.id == s.decoy && !d.popped; });
				if (it != g_decoys.end() && now < s.until) {
					// Keep the head on it: Elden Ring's own turning may pull it back toward its patrol.
					if (Cfg().decoyTurn && now - s.lastTurn >= 500) {
						for (const auto& c : chars) {
							if (c.handle == s.handle && c.chr == s.chr) {
								Turn(s.chr, c.pos, it->pos);
								break;
							}
						}
						s.lastTurn = now;
					}
					return false;
				}
				Release(s, chars);
				return true;
			});
		}
		std::erase_if(g_cooldown, [&](const auto& a_kv) { return now > a_kv.second; });
		Draw(a_f, now);
	}

	void OnPuppetStop()
	{
		ReleaseStarers(0, "the puppet stopped");
		if (!g_decoys.empty()) {
			Log::Info("decoys: puppet stopped; {} decoy(s) kept where they stand", g_decoys.size());
		}
		RenderMarkers::Set(RenderMarkers::Owner::kDecoys, {});
	}

	void OnWorldShift(const Vec3& a_delta)
	{
		for (auto& d : g_decoys) {
			d.pos += a_delta;
		}
	}

	std::string Describe()
	{
		return std::format("decoys: {} ({}): {} set, {} standing, {} stares (+{} heard), {} popped ({} shocks), {} picked up, {} turns, {} unknown re-announcements, "
		                   "{} dropped with a load; {} staring now",
			Cfg().decoys ? "on" : "off", Cfg().decoyTurn ? "turning starers" : "stare only", g_placed, g_decoys.size(), g_stares, g_heard, g_pops, g_shocks, g_gone,
			g_turns, g_ignored, g_dropped, g_starers.size());
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		if (a_w.size() > 1 && a_w[1] == "add") {
			// A test decoy without MGSV: m ahead of the Tarnished (default 6), facing him. [variant: plain|active|shock]
			if (!a_f.player || !Puppet::Active()) {
				return "decoys add: needs the puppet running";
			}
			const float m = a_w.size() > 2 ? std::strtof(a_w[2].c_str(), nullptr) : 6.0f;
			const std::string kind = a_w.size() > 3 ? a_w[3] : "plain";
			Vec3 fwd = Rotate(game::Orientation(a_f.player), { 0, 0, 1 }) * static_cast<float>(Calibration::ErForward());
			fwd = LengthXZ(fwd) > 1e-3f ? Normalize(Vec3{ fwd.x, 0, fwd.z }) : Vec3{ 0, 0, 1 };
			Decoy d;
			d.id = g_consoleNext++;
			d.equip = kind == "active" ? 401 : kind == "shock" ? 405 : 397;
			d.variant = decoy::VariantOf(d.equip);
			d.pos = game::Position(a_f.player) + fwd * (std::isfinite(m) ? std::clamp(m, 1.0f, 40.0f) : 6.0f);
			d.facing = -fwd;
			d.placed = ::GetTickCount64();
			Add(a_f, std::move(d));
			return Describe();
		}
		if (a_w.size() > 2 && a_w[1] == "pop") {
			const auto id = static_cast<std::uint32_t>(std::strtoul(a_w[2].c_str(), nullptr, 0));
			auto       it = std::ranges::find_if(g_decoys, [&](const Decoy& d) { return d.id == id; });
			if (it == g_decoys.end()) {
				return std::format("no decoy {}", id);
			}
			Pop(a_f, *it, "console");
			return Describe();
		}
		std::string out = Describe();
		const auto  now = ::GetTickCount64();
		for (const auto& d : g_decoys) {
			out += std::format("\n {:#x} {} equip {} at ({:.2f}, {:.2f}, {:.2f}), {:.1f} s old, {} stares{}", d.id, decoy::VariantName(d.variant), d.equip, d.pos.x,
				d.pos.y, d.pos.z, static_cast<double>(now - d.placed) / 1000.0, d.stares, d.popped ? " -- POPPED (" + d.why + ")" : "");
		}
		for (const auto& s : g_starers) {
			out += std::format("\n staring: {} at {:#x} ({:.1f} s left)", s.name, s.decoy, (static_cast<double>(s.until) - static_cast<double>(now)) / 1000.0);
		}
		return out;
	}
}
