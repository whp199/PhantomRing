#include "Cqc.h"

#include "Combat.h"
#include "Config.h"
#include "CqcLogic.h"
#include "Fulton.h"
#include "Game.h"
#include "Hud.h"
#include "Log.h"
#include "Mem.h"
#include "Puppet.h"

#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <optional>

#include <windows.h>

namespace phantom::er::Cqc
{
	namespace
	{
		constexpr std::uint8_t kHoldBits = 0x30;  // ChrIns+0x538: bit4 no attack, bit5 no move (Stealth's poster, the console's flags)

		struct Held
		{
			std::uint8_t* chr{ nullptr };
			std::uint64_t handle{ 0 };
			std::uint8_t  setBits{ 0 };  // the bits we set (that were clear): only these go back
			ULONGLONG     until{ 0 };
			std::string   name;
		};

		// A hit waiting for Combat's bullet row (cqc::WhenToFire).
		struct Queued
		{
			std::uint64_t     handle{ 0 };
			combat::HitEffect effect;
			float             reach{ 0 };
			ULONGLONG         asked{ 0 };
			std::string       what;
		};

		std::optional<Held>                                                        g_held;
		std::deque<Queued>                                                         g_queue;
		cqc::Cooldowns                                                             g_cooldown;  // no new hold before
		std::array<std::uint64_t, static_cast<std::size_t>(cqc::Response::kCount)> g_responses{};
		std::uint64_t g_events{ 0 }, g_missed{ 0 }, g_gags{ 0 }, g_noHold{ 0 }, g_late{ 0 }, g_lost{ 0 };
		std::string   g_last;

		Vec3 V(const float* a_v) { return { a_v[0], a_v[1], a_v[2] }; }

		cqc::Rules MakeRules()
		{
			const auto& c = Cfg();
			cqc::Rules  r;
			r.range = c.cqcRange;
			r.arcDeg = c.cqcArcDeg;
			r.bossBigness = c.cqcBossBigness;
			r.holdSeconds = c.cqcHoldSeconds;
			r.leaveDistance = c.cqcLeaveDistance;
			r.grabPoise = c.cqcGrabPoise;
			r.throwPoise = c.cqcThrowPoise;
			r.throwDmgLevel = static_cast<std::uint8_t>(std::clamp(c.cqcThrowDmgLevel, 0, 127));
			r.staggerPoise = c.cqcStaggerPoise;
			r.bossHp = c.cqcBossHp;
			r.holdCooldown = c.cqcHoldCooldown;
			r.hitCorrection = static_cast<std::uint16_t>(std::clamp(c.cqcHitCorrection, 0, 100));
			return r;
		}

		// The held enemy as Elden Ring has him now (the same character, alive), or nullptr.
		const game::Character* Find(const std::vector<game::Character>& a_chars, const Held& a_h)
		{
			for (const auto& c : a_chars) {
				if (c.chr == a_h.chr && c.handle == a_h.handle) {
					return &c;
				}
			}
			return nullptr;
		}

		// Our bits back on him: still the same character in Elden Ring's list (dead too: a corpse Elden Ring revives in place at
		// a rest would stand frozen), unless he is disabled or going up on a Fulton balloon -- then the bits are the lift's.
		bool PutBack(const game::Character* a_c, const Held& a_h)
		{
			if (!a_c || a_c->disabled || Fulton::IsLifting(a_h.handle)) {
				return false;
			}
			auto*      flags = a_h.chr + game::kChrDebugFlags;
			const auto v = SafeRead<std::uint8_t>(flags).value_or(0);
			SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(v & ~a_h.setBits));
			return true;
		}

		void Release(const char* a_why, const game::Character* a_c = nullptr)
		{
			if (!g_held) {
				return;
			}
			std::vector<game::Character> chars;
			if (!a_c) {
				chars = game::Characters(256, true);
				a_c = Find(chars, *g_held);
			}
			const bool back = PutBack(a_c, *g_held);
			// Any end of a hold: no new one on him for a while (release + re-grab kept him frozen ~6 s of every ~7).
			g_cooldown.Start(g_held->handle, ::GetTickCount64(), Cfg().cqcHoldCooldown);
			Log::Info("cqc: {} let go ({}){}", g_held->name, a_why, back ? "" : " -- his bits left alone (gone, disabled or a Fulton's)");
			g_held.reset();
		}

		Vec3 SnakeChest() { return Puppet::CurrentAnchor().pe + Vec3{ 0, 1.0f, 0 }; }  // as a strike's arc (Combat::OnMelee)

		void Enqueue(std::uint64_t a_handle, const combat::HitEffect& a_e, float a_reach, std::string a_what)
		{
			if (g_queue.size() >= 4) {
				++g_lost;
				Log::Info("cqc: {} dropped (four hits already wait for the bullet row)", g_queue.front().what);
				g_queue.pop_front();
			}
			g_queue.push_back({ a_handle, a_e, a_reach, ::GetTickCount64(), std::move(a_what) });
		}

		// The waiting hits, oldest first, each when the row is free (one per frame at most: the first one takes the row).
		void FireQueued(const Frame& a_f)
		{
			if (g_queue.empty()) {
				return;
			}
			const auto& c = Cfg();
			const auto  now = ::GetTickCount64();
			auto&       q = g_queue.front();
			const auto  when = cqc::WhenToFire(now, q.asked, static_cast<std::uint32_t>(std::max(0, c.cqcSettleMs)),
				 static_cast<std::uint32_t>(std::max(0, c.cqcRetryMs)), Combat::RowBusy());
			if (when == cqc::Wait::kWait) {
				return;
			}
			if (when == cqc::Wait::kFireLate) {
				++g_late;
				Log::Info("cqc: {} waited {} ms for the bullet row -- fired into a busy one (it may land nothing)", q.what, now - q.asked);
			}
			if (!Combat::HitCharacter(a_f, q.handle, SnakeChest(), q.reach, q.effect, q.what)) {
				++g_lost;
				Log::Info("cqc: {} -- he is out of reach now (or damage is off)", q.what);
			}
			g_queue.pop_front();
		}

		void Hold(const Combat::Foe& a_foe, const cqc::Rules& a_r)
		{
			if (g_held && g_held->handle != a_foe.handle) {
				Release("another one grabbed");
			}
			if (Fulton::IsLifting(a_foe.handle)) {
				return;  // the balloon has him (and his bits)
			}
			auto*      flags = a_foe.chr + game::kChrDebugFlags;
			const auto v = SafeRead<std::uint8_t>(flags);
			if (!v) {
				return;
			}
			Held h;
			h.chr = a_foe.chr;
			h.handle = a_foe.handle;
			h.name = a_foe.name;
			// A second grab of the one we hold keeps what we set the first time (his own bits stay his).
			h.setBits = g_held && g_held->handle == a_foe.handle ? g_held->setBits : static_cast<std::uint8_t>(kHoldBits & ~*v);
			h.until = ::GetTickCount64() + static_cast<ULONGLONG>(std::max(0.0f, a_r.holdSeconds) * 1000.0f);
			SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(*v | kHoldBits));
			g_held = h;
		}
	}

	void OnEvent(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		++g_events;
		const auto action = static_cast<proto::CqcAction>(a_e.a);
		if (a_e.a > proto::kCqcRelease) {
			Log::Warn("cqc: unknown CQC action {} (MGSV attack {})", a_e.a, a_e.pad[0]);
			return;
		}
		const auto& c = Cfg();
		if (!c.cqc || !Puppet::Active() || !a_f.player) {
			Log::Info("Snake CQC {} (MGSV attack {}) -- {}", cqc::Name(action), a_e.pad[0], !c.cqc ? "[Cqc] enabled = false" : "no puppet");
			return;
		}
		const auto rules = MakeRules();
		const auto& anchor = Puppet::CurrentAnchor();
		Vec3        facing = anchor.DirToEr(V(a_e.v1));
		facing.y = 0;
		const Vec3 snake = SnakeChest();
		if (action == proto::kCqcRelease) {
			++g_responses[static_cast<std::size_t>(cqc::Response::kRelease)];
			g_last = "release";
			Release("Snake let go");
			return;
		}
		// Who: the one Snake holds while he is still in reach, else (a grab, a throw) the nearest in front of him. A choke or an
		// interrogation never goes to whoever walked up while RT stayed down after a grab that caught nobody.
		const auto                  foes = Combat::HostilesNear(a_f, snake, std::max(rules.range, rules.leaveDistance) + 1.0f);
		std::vector<cqc::Candidate> candidates;
		std::optional<std::size_t>  pick;
		for (std::size_t i = 0; i < foes.size(); ++i) {
			candidates.push_back({ foes[i].cap, foes[i].maxHp, true });
			if (g_held && foes[i].handle == g_held->handle && combat::CapsuleDistance(foes[i].cap, snake) <= rules.leaveDistance) {
				pick = i;
			}
		}
		if (!pick && cqc::NeedsHold(action)) {
			++g_noHold;
			g_last = std::format("{}: no hold", cqc::Name(action));
			Log::Info("Snake CQC {} (MGSV attack {}): no hold -- nobody is held{}", cqc::Name(action), a_e.pad[0],
				g_held ? std::format(" within {:.1f} m ({} is further)", rules.leaveDistance, g_held->name) : std::string());
			return;
		}
		if (!pick) {
			pick = cqc::SelectTarget(snake, facing, candidates, rules);
		}
		if (!pick) {
			++g_missed;
			g_last = std::format("{}: nobody in reach", cqc::Name(action));
			Log::Info("Snake CQC {} (MGSV attack {}): no Elden Ring enemy within {:.1f} m in front of him", cqc::Name(action), a_e.pad[0], rules.range);
			return;
		}
		const auto&    foe = foes[*pick];
		const auto     hit = Combat::CurrentRules();
		const bool     boss = cqc::IsBoss(foe.maxHp, hit.scaling, rules);
		cqc::Situation sit;
		sit.boss = boss;
		sit.held = g_held && g_held->handle == foe.handle;
		sit.cooling = g_cooldown.Cooling(foe.handle, ::GetTickCount64());
		const auto resp = cqc::Respond(action, sit);
		++g_responses[static_cast<std::size_t>(resp)];
		g_last = std::format("{} -> {} on {}{}", cqc::Name(action), cqc::Name(resp), foe.name, boss ? " (boss)" : "");
		Log::Info("Snake CQC {} (MGSV attack {}) -> {} on {}{} (max hp {}, bigness {:.2f})", cqc::Name(action), a_e.pad[0], cqc::Name(resp), foe.name,
			boss ? " -- a boss: stagger only" : "", foe.maxHp, combat::Bigness(foe.maxHp, hit.scaling));
		const auto what = std::format("CQC {}", cqc::Name(resp));
		const auto effect = cqc::EffectOf(resp, foe.maxHp, Combat::CurrentMelee(), hit, rules);
		// The hits wait for Combat's bullet row (FireQueued): MGSV's punch of the same press usually holds it right now.
		switch (resp) {
		case cqc::Response::kHold:
			Hold(foe, rules);
			Enqueue(foe.handle, effect, rules.range + 0.5f, what);
			break;
		case cqc::Response::kFlinch:
			Enqueue(foe.handle, effect, rules.range + 0.5f, what);
			break;
		case cqc::Response::kKnockdown:
			if (g_held && g_held->handle == foe.handle) {
				Release("thrown");  // his own reaction must play: nothing holds him through the fall
			}
			Enqueue(foe.handle, effect, rules.range + 0.5f, what);
			break;
		case cqc::Response::kSleep:
		case cqc::Response::kStagger:
			Enqueue(foe.handle, effect, rules.leaveDistance, what);
			break;
		case cqc::Response::kGag:
			if (c.cqcInterrogate) {
				Hud::Notify(std::format("Interrogation: {}", cqc::GagLine(static_cast<std::uint32_t>(g_gags++))), Hud::Tone::kInfo);
			}
			break;
		default:
			break;
		}
	}

	void Tick(const Frame& a_f)
	{
		const auto now = ::GetTickCount64();
		g_cooldown.Expire(now);
		if (!Puppet::Active() || !a_f.player) {
			g_queue.clear();
			Release("the puppet stopped");
			return;
		}
		FireQueued(a_f);
		if (!g_held) {
			return;
		}
		if (Fulton::IsLifting(g_held->handle)) {
			// The balloon took him: its hold bits are the same ones and it keeps them until he is out of the world.
			Log::Info("cqc: {} goes up on a Fulton balloon -- the lift holds him now", g_held->name);
			g_held.reset();
			return;
		}
		const auto  chars = game::Characters(256, true);
		const auto* h = Find(chars, *g_held);
		if (!h || h->dead || h->disabled) {
			Release(!h ? "gone" : h->dead ? "dead" : "disabled", h);  // a corpse still gets his bits back (PutBack)
			return;
		}
		if (now >= g_held->until) {
			Release("hold_seconds", h);
			return;
		}
		const Vec3 snake = game::Position(a_f.player);
		const Vec3 d = h->pos - snake;
		if (std::hypot(d.x, d.z) > Cfg().cqcLeaveDistance) {
			Release("Snake walked away", h);
			return;
		}
		// Someone else cleared them under us (a poster stare that ended, the console's flags): ours again, and ours to put back.
		auto* flags = g_held->chr + game::kChrDebugFlags;
		if (const auto v = SafeRead<std::uint8_t>(flags); v && (*v & kHoldBits) != kHoldBits) {
			g_held->setBits |= static_cast<std::uint8_t>(kHoldBits & ~*v);
			SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(*v | kHoldBits));
		}
	}

	void OnPuppetStop()
	{
		g_queue.clear();
		Release("the puppet stopped");
	}

	bool IsHeld(std::uint64_t a_handle) { return g_held && g_held->handle == a_handle; }

	std::string Describe()
	{
		using R = cqc::Response;
		const auto n = [](R a_r) { return g_responses[static_cast<std::size_t>(a_r)]; };
		return std::format("cqc: {} (range {:.1f} m, arc {:.0f} deg, boss from bigness {:.2f} or {} hp, hit correction {}%); {} events: hold {}, "
		                   "knockdown {}, sleep {}, interrogation {}, release {}, boss stagger {}, cooldown flinch {}, nobody in reach {}, no hold {}; "
		                   "hits waiting {}, fired late {}, lost {}; cooling {}; holding: {}; last: {}",
			Cfg().cqc ? "on" : "OFF ([Cqc] enabled = false: events only logged)", Cfg().cqcRange, Cfg().cqcArcDeg, Cfg().cqcBossBigness, Cfg().cqcBossHp,
			Cfg().cqcHitCorrection, g_events, n(R::kHold), n(R::kKnockdown), n(R::kSleep), n(R::kGag), n(R::kRelease), n(R::kStagger), n(R::kFlinch), g_missed,
			g_noHold, g_queue.size(), g_late, g_lost, g_cooldown.Size(),
			g_held ? std::format("{} ({:.1f} s left)", g_held->name, (static_cast<double>(g_held->until) - static_cast<double>(::GetTickCount64())) / 1000.0)
			       : std::string("nobody"),
			g_last.empty() ? "none" : g_last);
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		if (a_w.size() < 2 || a_w[1] == "status") {
			return Describe();
		}
		if (a_w[1] == "on" || a_w[1] == "off") {
			Cfg().cqc = a_w[1] == "on";
			if (!Cfg().cqc) {
				g_queue.clear();
				Release("console: off");
			}
			return Describe();
		}
		if (a_w[1] == "set" && a_w.size() > 3) {
			// Live tuning (not saved): which dmgLevel is Elden Ring's knockdown is the first thing to try.
			auto&       c = Cfg();
			const float v = std::strtof(a_w[3].c_str(), nullptr);
			if (a_w[2] == "throw_dmg_level") {
				c.cqcThrowDmgLevel = static_cast<int>(v);
			} else if (a_w[2] == "throw_poise") {
				c.cqcThrowPoise = v;
			} else if (a_w[2] == "grab_poise") {
				c.cqcGrabPoise = v;
			} else if (a_w[2] == "stagger_poise") {
				c.cqcStaggerPoise = v;
			} else if (a_w[2] == "range") {
				c.cqcRange = v;
			} else if (a_w[2] == "hold_seconds") {
				c.cqcHoldSeconds = v;
			} else if (a_w[2] == "hold_cooldown") {
				c.cqcHoldCooldown = v;
			} else if (a_w[2] == "hit_correction") {
				c.cqcHitCorrection = static_cast<int>(v);
			} else if (a_w[2] == "boss_hp") {
				c.cqcBossHp = static_cast<int>(v);
			} else if (a_w[2] == "settle_ms") {
				c.cqcSettleMs = static_cast<int>(v);
			} else if (a_w[2] == "retry_ms") {
				c.cqcRetryMs = static_cast<int>(v);
			} else {
				return "cqc set throw_dmg_level|throw_poise|grab_poise|stagger_poise|range|hold_seconds|hold_cooldown|hit_correction|boss_hp|settle_ms|retry_ms <value>";
			}
			return Describe() + std::format("\n throw: poise {:.0f} dmgLevel {}, hit correction {}%", c.cqcThrowPoise, c.cqcThrowDmgLevel, c.cqcHitCorrection);
		}
		static constexpr std::array<std::pair<const char*, proto::CqcAction>, 6> kNames{ {
			{ "grab", proto::kCqcGrab },
			{ "hold", proto::kCqcHold },
			{ "throw", proto::kCqcThrow },
			{ "choke", proto::kCqcChoke },
			{ "interrogate", proto::kCqcInterrogate },
			{ "release", proto::kCqcRelease },
		} };
		for (const auto& [name, action] : kNames) {
			if (a_w[1] == name) {
				// As the guest sends it: Snake's MGSV root and facing (the anchor maps them back to the puppet).
				proto::GuestEvent e{};
				e.type = proto::kGEvCqc;
				e.a = action;
				e.b = static_cast<std::uint32_t>(Cfg().cqcRange * 1000.0f);
				std::copy_n(a_f.guest.rootPos, 3, e.v0);
				e.v1[0] = std::sin(a_f.guest.rootYaw);
				e.v1[2] = std::cos(a_f.guest.rootYaw);
				OnEvent(a_f, e);
				return Describe();
			}
		}
		return "usage: cqc [status | grab | hold | throw | choke | interrogate | release | on | off | set <key> <value>]";
	}
}
