#include "Marking.h"

#include "Calibration.h"
#include "Combat.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Log.h"
#include "MarkingLogic.h"
#include "Mem.h"
#include "Puppet.h"
#include "RenderMarkers.h"

#include "../protocol/phantomring_protocol.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <numbers>
#include <optional>
#include <utility>
#include <vector>

namespace phantom::er::Marking
{
	namespace
	{
		constexpr float kCentreDeg = 10.0f;      // distance = centre: marks this near the view's centre show it
		constexpr float kAimHeight = 1.2f;       // m over the feet: the chest the cone and the ray aim at
		// CSChrPhysicsModule hit capsule (NpcParam hitHeight/hitRadius), then the character-vs-character one: the offsets
		// er/Combat.cpp reads for its targets (fromsoftware-rs physics.rs [V-src], sanity-checked per character there too).
		constexpr std::uint32_t kPhysHitHeight = 0x2F0;
		constexpr std::uint32_t kPhysHitRadius = 0x2F4;
		constexpr std::uint32_t kPhysChrHitHeight = 0x2E0;
		constexpr std::uint32_t kPhysChrHitRadius = 0x2E4;
		constexpr std::uint8_t  kRenderBit = 0x08;  // ChrIns+0x1C5 bit3 enable_render (er/Game.h; UNVERIFIED as "in the world")

		marking::Marks     g_marks;
		marking::LoadWatch g_loads;
		std::string    g_force{ "auto" };   // mark binoculars on|off|auto
		bool           g_drawn{ false };    // markers were set last frame
		bool           g_wasLooking{ false };
		bool           g_noRayLogged{ false };
		std::uint64_t  g_marked{ 0 }, g_frames{ 0 }, g_blocked{ 0 };
		std::size_t    g_unrendered{ 0 };   // characters the last look skipped as not rendered (status)

		marking::Rules Rules()
		{
			const auto&    c = Cfg().marking;
			marking::Rules r;
			r.range = c.range;
			r.coneShare = c.cone;
			r.minConeDeg = c.minConeDeg;
			r.dwell = c.dwell;
			r.fade = c.fade;
			r.maxMarked = static_cast<std::size_t>(std::max(1, c.maxMarked));
			return r;
		}

		Vec3 Row(const std::uint8_t* a_cam, int a_row)
		{
			const auto* f = reinterpret_cast<const float*>(a_cam + game::kCamMatrix + 16 * a_row);
			return { f[0], f[1], f[2] };
		}

		// Elden Ring's render camera (what the player sees: MGSV's binoculars while Snake looks through them).
		bool ReadCamera(marking::Camera& a_out)
		{
			const auto* cam = game::PersCam1();
			if (!cam) {
				return false;
			}
			a_out.right = Normalize(Row(cam, 0));
			a_out.up = Normalize(Row(cam, 1));
			a_out.fwd = Normalize(Row(cam, 2) * Calibration::CamForwardRow());
			a_out.pos = Row(cam, 3);
			a_out.fovY = *reinterpret_cast<const float*>(cam + game::kCamFov);
			return Finite(a_out.right) && Finite(a_out.up) && Finite(a_out.fwd) && Finite(a_out.pos) && a_out.fovY > 0.01f && a_out.fovY < 3.0f;
		}

		void Hide()
		{
			if (g_drawn) {
				RenderMarkers::Set(RenderMarkers::Owner::kMarking, {});
				g_drawn = false;
			}
		}

		bool Looking(const Frame& a_f)
		{
			if (g_force == "on") {
				return true;
			}
			if (g_force == "off") {
				return false;
			}
			return (a_f.guest.flags & proto::kGuestBinoculars) && !(a_f.guest.flags & proto::kGuestInMenu);
		}

		struct Chr
		{
			game::Character                c;
			bool                           hostile{ false };
			bool                           alive{ false };
			std::optional<combat::Capsule> body;
		};

		bool SaneCapsule(float a_height, float a_radius)
		{
			return std::isfinite(a_height) && std::isfinite(a_radius) && a_radius >= 0.08f && a_radius <= 10.0f && a_height >= 0.2f && a_height <= 40.0f;
		}

		// His hit capsule, as er/Combat.cpp finds it; none when neither of the physics module's capsules is sane.
		std::optional<combat::Capsule> Body(const game::Character& a_c)
		{
			auto* modules = SafePtr(a_c.chr + game::kChrModules);
			auto* phys = modules ? SafePtr(modules + game::kModPhysics) : nullptr;
			if (!phys) {
				return std::nullopt;
			}
			for (const auto& [hOff, rOff] : { std::pair{ kPhysHitHeight, kPhysHitRadius }, std::pair{ kPhysChrHitHeight, kPhysChrHitRadius } }) {
				const float h = SafeRead<float>(phys + hOff).value_or(NAN);
				const float r = SafeRead<float>(phys + rOff).value_or(NAN);
				if (SaneCapsule(h, r)) {
					return combat::ChrCapsule(a_c.pos, h, r);
				}
			}
			return std::nullopt;
		}

		// The characters Snake could mark or sees marked. a_bodies: read their hit capsules too (only while looking).
		std::vector<Chr> Around(const Frame& a_f, bool a_bodies)
		{
			const auto&      c = Cfg().marking;
			const auto       myTeam = SafeRead<std::uint8_t>(a_f.player.chr + game::kChrTeam).value_or(0);
			std::vector<Chr> out;
			std::size_t      unrendered = 0;
			for (const auto& ch : game::Characters(static_cast<std::size_t>(std::max(1, c.maxCharacters)) + 1)) {
				if (ch.isPlayer || ch.chr == a_f.player.chr || !Finite(ch.pos)) {
					continue;
				}
				// Elden Ring keeps invisible characters in its world: "c1000" dummy shooters behind ballistas and arrow traps,
				// ambushers and NPCs an event has hidden until a trigger. They have no collision, so the ray passes: never mark
				// one, and keep a marked one's mark out of sight while an event hides him (he shows again once he is back).
				// The dead stay (their mark fades). UNVERIFIED live: that enable_render is clear exactly for those.
				const bool rendered = (SafeRead<std::uint8_t>(ch.chr + game::kChrFlags1C5).value_or(kRenderBit) & kRenderBit) != 0;
				if (c.renderedOnly && !rendered && !ch.dead) {
					++unrendered;
					continue;
				}
				if (c.renderedOnly && !ch.dead && ch.maxHp <= 0) {
					continue;  // no HP at all: not a character anyone fights or talks to (Combat::Targets skips them too)
				}
				Chr x{ ch };
				// Down to 0 HP counts as dead before the death bit is set (the mark starts fading at the killing blow).
				x.alive = !ch.dead && ch.hp > 0;
				if (a_bodies) {
					x.body = Body(ch);
				}
				// The ActorTable's kActorHostile rule (er/Actors.cpp): the same characters MGSV would see as soldiers.
				x.hostile = ch.isEnemy && ch.team != myTeam && Combat::HostileToPlayer(a_f, ch.team);
				if (c.targets == "hostile" && !x.hostile) {
					continue;
				}
				out.push_back(x);
			}
			g_unrendered = unrendered;
			return out;
		}
	}

	void Tick(const Frame& a_f)
	{
		const auto& c = Cfg().marking;
		if (!c.enabled) {
			Hide();
			return;
		}
		// A new world (after a load screen, another character): its characters are not the ones marked (handles come back).
		const auto nowMs = static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		if (g_loads.Update(a_f.inGame, reinterpret_cast<std::uintptr_t>(a_f.player.chr), nowMs) && (g_marks.Count() || g_marks.Looking())) {
			Log::Info("marking: a new world (load or another character): {} marks forgotten", g_marks.Count());
			g_marks.Clear();
		}
		// Marks are Snake's: none for the Tarnished, none while Elden Ring or MGSV owns the screen (Elden Ring's menus, a grace's,
		// a talk: menuOpen / interacting; MGSV's iDroid or pause menu: kGuestInMenu, as Fulton's markers). Not Elden Ring's
		// HUD hiding: with its "Auto" HUD it hides out of combat, which is exactly when Snake scouts through the binoculars.
		const bool show = Puppet::Active() && a_f.guestReady && a_f.inGame && a_f.player && !a_f.loading && !a_f.menuOpen && !a_f.interacting &&
		                  !a_f.cutscene && !a_f.dead && !a_f.dying && !(a_f.guest.flags & proto::kGuestInMenu);
		if (!show) {
			Hide();
			return;
		}
		const bool looking = Looking(a_f);
		if (looking != g_wasLooking) {
			Log::Info("marking: binoculars {} ({} marked)", looking ? "up" : "down", g_marks.Count());
			g_wasLooking = looking;
		}
		if (!looking && g_marks.Count() == 0 && g_marks.Looking() == 0) {
			Hide();
			return;
		}
		marking::Camera cam;
		if (!ReadCamera(cam)) {
			Hide();
			return;
		}
		++g_frames;
		const auto chrs = Around(a_f, looking);
		std::vector<marking::Seen> seen;
		seen.reserve(chrs.size());
		for (const auto& x : chrs) {
			seen.push_back({ x.c.handle, x.c.pos + Vec3{ 0, kAimHeight, 0 }, x.alive, x.body });
		}
		const marking::View view{ cam.pos, cam.fwd, cam.fovY, looking };
		const bool          rays = c.lineOfSight && game::CanCastRay();
		if (c.lineOfSight && !rays && looking && !g_noRayLogged) {
			g_noRayLogged = true;
			Log::Warn("marking: Elden Ring's ray cast is not available; marking without the line-of-sight check");
		}
		const auto los = [&](const Vec3& a_from, const marking::Seen& a_target) {
			if (!rays) {
				return true;
			}
			Vec3 hit;
			if (!game::CastRay(a_from, a_target.aim - a_from, hit, a_f.player.chr, Cfg().rayFilter)) {
				return true;
			}
			// A ray stopping on his own capsule still sees him (Combat.cpp: Elden Ring's ray may stop on the body it hit).
			const bool clear = marking::RayClear(a_from, a_target.aim, hit, a_target.body);
			g_blocked += clear ? 0 : 1;
			return clear;
		};
		const auto rules = Rules();
		for (const auto h : g_marks.Update(view, seen, a_f.dt, rules, los)) {
			++g_marked;
			const auto it = std::find_if(chrs.begin(), chrs.end(), [&](const Chr& x) { return x.c.handle == h; });
			if (it != chrs.end()) {
				Log::Info("marking: marked 0x{:x} (npc {}, {}) at {:.0f} m", h, it->c.npcParam, it->hostile ? "hostile" : "not hostile", Length(it->c.pos - cam.pos));
			}
		}

		marking::Style style;
		style.size = c.size;
		style.textSize = c.textSize;
		const Vec3          snake = game::Position(a_f.player);
		std::vector<Marker> out;
		const float         centre = kCentreDeg * std::numbers::pi_v<float> / 180.0f;
		for (const auto& x : chrs) {
			const float op = g_marks.Opacity(x.c.handle, rules.fade);
			if (op <= 0.0f) {
				continue;
			}
			const Vec3  at = x.c.pos + Vec3{ 0, c.height, 0 };
			std::string text;
			if (c.distance == "always" || (c.distance == "centre" && marking::OffAxis(view, at, marking::Rules{ .bodyRadius = 0.0f }) <= centre)) {
				text = marking::DistanceText(Length(x.c.pos - snake));
			}
			marking::Build(out, cam, at, text, x.hostile, op, g_marks.Age(x.c.handle), style);
		}
		if (out.empty()) {
			Hide();
			return;
		}
		RenderMarkers::Set(RenderMarkers::Owner::kMarking, std::move(out));
		g_drawn = true;
	}

	std::string Describe()
	{
		return std::format("marking: {} | binoculars {} | {} marked, {} being looked at | {} marked this session, {} rays ({} blocked), {} frames, "
		                   "{} unrendered skipped",
			Cfg().marking.enabled ? "on" : "off", g_force, g_marks.Count(), g_marks.Looking(), g_marked, g_marks.Rays(), g_blocked, g_frames, g_unrendered);
	}

	std::string Command(const Frame& a_f, std::string_view a_args)
	{
		std::vector<std::string> w;
		for (std::size_t i = 0; i < a_args.size();) {
			const auto j = a_args.find(' ', i);
			const auto n = j == std::string_view::npos ? a_args.size() : j;
			if (n > i) {
				w.emplace_back(a_args.substr(i, n - i));
			}
			i = n + 1;
		}
		if (w.empty() || w[0] == "status") {
			return Describe();
		}
		if (w[0] == "binoculars" && w.size() > 1 && (w[1] == "on" || w[1] == "off" || w[1] == "auto")) {
			g_force = w[1];
			return std::format("marking: binoculars {} ({})", g_force, g_force == "auto" ? "MGSV's kGuestBinoculars" : "forced");
		}
		if (w[0] == "clear") {
			const auto n = g_marks.Count();
			g_marks.Clear();
			return std::format("marking: {} marks forgotten", n);
		}
		if (w[0] == "nearest") {
			if (!a_f.player) {
				return "marking: no player";
			}
			for (const auto& x : Around(a_f, false)) {
				if (x.alive) {
					const bool fresh = g_marks.Mark(x.c.handle, Rules());
					g_marked += fresh ? 1 : 0;
					return std::format("marking: 0x{:x} (npc {}, {:.1f} m, {}) {}", x.c.handle, x.c.npcParam, x.c.distance, x.hostile ? "hostile" : "not hostile",
						fresh ? "marked" : "was marked already");
				}
			}
			return "marking: no living character around";
		}
		return "mark [status | binoculars on|off|auto | nearest | clear]";
	}
}
