#include "Stealth.h"

#include "Addresses.h"
#include "Calibration.h"
#include "Combat.h"
#include "Config.h"
#include "Cqc.h"
#include "Game.h"
#include "Log.h"
#include "Mem.h"
#include "Puppet.h"

#include "phantomring_protocol.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <numbers>

#include <windows.h>

namespace phantom::er::Stealth
{
	namespace
	{
		using ApplySpEffectFn = void (*)(void* a_chr, std::int32_t a_id, bool a_dontSync);
		using RemoveSpEffectFn = void (*)(void* a_chr, std::int32_t a_id);
		constexpr std::uint32_t kChrSpEffect = 0x178;  // ChrIns -> SpecialEffect: +8 first entry; entry +8 id, +0x30 next (fromsoftware-rs)
		constexpr std::uint8_t  kStareBits = 0x30;     // ChrIns+0x538: bit4 no attack, bit5 no move

		struct Starer
		{
			std::uint8_t* chr{ nullptr };
			std::uint64_t handle{ 0 };
			std::uint8_t  setBits{ 0 };  // the bits we set (that were clear)
			ULONGLONG     until{ 0 };
			std::string   name;
		};

		BoxState                         g_box;
		bool                             g_hidden{ false };
		std::uint8_t*                    g_hiddenChr{ nullptr };
		std::int32_t                     g_effect{ 0 };       // the SpEffect id we applied (to take off the same one)
		ULONGLONG                        g_lastCheck{ 0 };
		ULONGLONG                        g_lastPoster{ 0 };
		bool                             g_wasInBox{ false };
		std::vector<Starer>              g_starers;
		std::map<std::uint64_t, ULONGLONG> g_cooldown;        // handle -> no stare before
		std::uint64_t g_hides{ 0 }, g_reveals{ 0 }, g_reapplied{ 0 }, g_stares{ 0 }, g_boxes{ 0 };
		std::string   g_lastWhy;

		bool HasEffect(std::uint8_t* a_chr, std::int32_t a_id)
		{
			auto* module = SafePtr(a_chr + kChrSpEffect);
			auto* e = module ? SafePtr(module + 8) : nullptr;
			for (int i = 0; e && i < 128; ++i) {
				if (SafeRead<std::int32_t>(e + 8).value_or(-1) == a_id) {
					return true;
				}
				e = SafePtr(e + 0x30);
			}
			return false;
		}

		bool Usable()
		{
			return Addr().HasAll(A::ApplySpEffect, A::RemoveSpEffect) && Cfg().stealthSpEffect > 0;
		}

		void Hide(const Frame& a_f, bool a_on, std::string_view a_why)
		{
			const auto& a = Addr();
			if (a_on && !g_hidden) {
				g_effect = Cfg().stealthSpEffect;
				a.Fn<ApplySpEffectFn>(A::ApplySpEffect)(a_f.player.chr, g_effect, true);
				g_hidden = true;
				g_hiddenChr = a_f.player.chr;
				++g_hides;
				Log::Info("stealth: Snake hidden in the box ({}) -- SpEffect {} on the Tarnished: {}", a_why, g_effect,
					HasEffect(a_f.player.chr, g_effect) ? "active" : "NOT in its effect list");
			} else if (!a_on && g_hidden) {
				// Only on the same, live character (a load or another player object: the effect went with it).
				if (a_f.player && a_f.player.chr == g_hiddenChr) {
					a.Fn<RemoveSpEffectFn>(A::RemoveSpEffect)(a_f.player.chr, g_effect);
				}
				g_hidden = false;
				g_hiddenChr = nullptr;
				++g_reveals;
				Log::Info("stealth: Snake seen again ({}) -- SpEffect {} taken off", a_why, g_effect);
			}
			g_lastWhy = a_why;
		}

		// The bits we set go back; the rest is the game's.
		void Release(Starer& a_s, const std::vector<game::Character>& a_chars)
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

		void ReleaseAll(const char* a_why)
		{
			if (g_starers.empty()) {
				return;
			}
			const auto chars = game::Characters(256);
			for (auto& s : g_starers) {
				Release(s, chars);
			}
			Log::Info("stealth: {} poster starer(s) released ({})", g_starers.size(), a_why);
			g_starers.clear();
		}

		// Enemies that can see the poster box stop and stare for a moment.
		void Poster(const Frame& a_f, const Vec3& a_box)
		{
			const auto& c = Cfg();
			const auto  now = ::GetTickCount64();
			if (now - g_lastPoster < 250) {
				return;
			}
			g_lastPoster = now;
			const float cone = std::cos(70.0f * std::numbers::pi_v<float> / 180.0f);
			for (const auto& foe : Combat::HostilesNear(a_f, a_box, c.boxPosterRange)) {
				if (std::ranges::any_of(g_starers, [&](const Starer& s) { return s.handle == foe.handle; })) {
					continue;
				}
				if (Cqc::IsHeld(foe.handle)) {
					continue;  // CQC owns his no-attack / no-move bits: a stare's end must not let him go
				}
				if (auto it = g_cooldown.find(foe.handle); it != g_cooldown.end() && now < it->second) {
					continue;
				}
				const Vec3 eye = foe.cap.b;  // top sphere centre: about the eyes
				Vec3       to = a_box - eye;
				const float d = Length(to);
				if (d < 0.5f || d > c.boxPosterRange) {
					continue;
				}
				Vec3 fwd = Rotate(foe.orientation, { 0, 0, 1 }) * static_cast<float>(Calibration::ErForward());
				fwd.y = 0;
				Vec3 flat{ to.x, 0, to.z };
				if (LengthXZ(fwd) < 1e-3f || LengthXZ(flat) < 1e-3f || Dot(Normalize(fwd), Normalize(flat)) < cone) {
					continue;  // the box is behind it
				}
				Vec3 hit;
				if (game::CastRay(eye, to * 0.97f, hit, a_f.player.chr, c.rayFilter)) {
					continue;  // a wall in between
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
				s.until = now + static_cast<ULONGLONG>(std::max(0.5f, c.boxPosterStare) * 1000.0f);
				s.name = foe.name;
				SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(*v | kStareBits));
				g_starers.push_back(s);
				g_cooldown[foe.handle] = s.until + static_cast<ULONGLONG>(std::max(0.0f, c.boxPosterCooldown) * 1000.0f);
				++g_stares;
				Log::Info("stealth: {} sees the poster box {:.1f} m away and stops to stare ({:.0f} s)", foe.name, d, c.boxPosterStare);
			}
		}
	}

	bool BoxHidden(BoxState& a_s, bool a_inBox, float a_speed, float a_nearest, std::uint64_t a_nowMs, const BoxRules& a_r)
	{
		if (!a_inBox) {
			a_s.stillMs = 0;
			return false;
		}
		if (a_speed <= a_r.stillSpeed) {
			a_s.stillMs = a_nowMs;
		}
		if (a_nearest <= a_r.revealDistance) {
			return false;  // close enough to see through the box
		}
		if (a_speed <= a_r.stillSpeed) {
			return true;
		}
		// Creeping: hidden for a moment after he last kept still, then not.
		return a_speed <= a_r.slowSpeed && a_s.stillMs != 0 && static_cast<float>(a_nowMs - a_s.stillMs) <= a_r.slowGraceMs;
	}

	void Tick(const Frame& a_f)
	{
		const auto& c = Cfg();
		const bool  puppet = Puppet::Active() && a_f.player && a_f.guestReady && !a_f.dead && !a_f.dying;
		const bool  inBox = puppet && (a_f.guest.flags & proto::kGuestInBox) != 0;
		if (inBox != g_wasInBox) {
			g_wasInBox = inBox;
			if (inBox) {
				++g_boxes;
			}
			Log::Info("stealth: Snake {} the box{}", inBox ? "is in" : "left", inBox && (a_f.guest.flags & proto::kGuestBoxPoster) ? " (with a poster)" : "");
		}
		if (!puppet || !c.boxHide || !Usable()) {
			if (g_hidden) {
				Hide(a_f, false, !puppet ? "Snake is not playing" : "box hiding off");
			}
			ReleaseAll("Snake is not in the box");
			g_box = {};
			return;
		}
		const auto  now = ::GetTickCount64();
		const Vec3  feet = game::Position(a_f.player);
		const float speed = std::hypot(a_f.guest.velocity[0], a_f.guest.velocity[2]);
		float       nearest = 1e9f;
		if (inBox) {
			for (const auto& foe : Combat::HostilesNear(a_f, feet, c.boxRevealDistance + 1.0f)) {
				nearest = std::min(nearest, std::max(0.0f, combat::CapsuleDistance(foe.cap, feet + Vec3{ 0, 0.5f, 0 })));
			}
		}
		BoxRules rules;
		rules.stillSpeed = c.boxStillSpeed;
		rules.slowSpeed = c.boxSlowSpeed;
		rules.slowGraceMs = c.boxSlowGrace * 1000.0f;
		rules.revealDistance = c.boxRevealDistance;
		const bool hide = BoxHidden(g_box, inBox, speed, nearest, now, rules);
		if (hide != g_hidden) {
			Hide(a_f, hide,
				!inBox                            ? "out of the box"
				: nearest <= c.boxRevealDistance ? "an enemy right next to the box"
				: hide                           ? (speed <= c.boxStillSpeed ? "keeping still" : "creeping")
				                                 : "moving");
		} else if (g_hidden && now - g_lastCheck > 1000) {
			// Elden Ring may drop it (a load, a grace rest): put it back.
			g_lastCheck = now;
			if (!HasEffect(a_f.player.chr, g_effect)) {
				Addr().Fn<ApplySpEffectFn>(A::ApplySpEffect)(a_f.player.chr, g_effect, true);
				++g_reapplied;
			}
		}
		// The poster: enemies that see it stare (whether or not they could see Snake).
		if (inBox && (a_f.guest.flags & proto::kGuestBoxPoster) && c.boxPoster) {
			Poster(a_f, feet + Vec3{ 0, 0.5f, 0 });
		}
		if (!g_starers.empty()) {
			const bool keep = inBox && (a_f.guest.flags & proto::kGuestBoxPoster);
			const auto chars = game::Characters(256);
			std::erase_if(g_starers, [&](Starer& s) {
				if (keep && now < s.until) {
					return false;
				}
				Release(s, chars);
				return true;
			});
		}
		std::erase_if(g_cooldown, [&](const auto& a_kv) { return now > a_kv.second; });
	}

	void OnPuppetStop(const Frame& a_f)
	{
		if (g_hidden) {
			Hide(a_f, false, "the puppet stopped");
		}
		ReleaseAll("the puppet stopped");
	}

	std::string Describe()
	{
		return std::format("stealth: box hiding {} (SpEffect {}{}), Snake {}{}; {} boxes, hidden {} times, seen {} times, effect re-applied {}; poster: {} stares, {} "
		                   "staring now",
			Cfg().boxHide ? "on" : "off", Cfg().stealthSpEffect, Usable() ? "" : ", UNUSABLE: ApplySpEffect/RemoveSpEffect unverified",
			g_hidden ? "HIDDEN" : (g_wasInBox ? "in the box, visible" : "not in a box"), g_lastWhy.empty() ? "" : " (" + g_lastWhy + ")", g_boxes, g_hides, g_reveals,
			g_reapplied, g_stares, g_starers.size());
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		if (a_w.size() > 2 && a_w[1] == "speffect") {
			// Try another SpEffect row live (not saved); the current one comes off first.
			if (g_hidden) {
				Hide(a_f, false, "console");
			}
			Cfg().stealthSpEffect = static_cast<std::int32_t>(std::strtol(a_w[2].c_str(), nullptr, 10));
			return Describe();
		}
		if (a_w.size() > 2 && a_w[1] == "hide") {
			Cfg().boxHide = a_w[2] == "on";
			return Describe();
		}
		std::string out = Describe();
		for (const auto& s : g_starers) {
			out += std::format("\n staring: {} ({:.1f} s left)", s.name, (static_cast<double>(s.until) - static_cast<double>(::GetTickCount64())) / 1000.0);
		}
		if (a_f.player) {
			out += std::format("\n the Tarnished carries SpEffect {}: {}", Cfg().stealthSpEffect, HasEffect(a_f.player.chr, Cfg().stealthSpEffect) ? "yes" : "no");
		}
		return out;
	}
}
