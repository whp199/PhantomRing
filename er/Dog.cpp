#include "Dog.h"

#include "Buddy.h"
#include "Calibration.h"
#include "Combat.h"
#include "Config.h"
#include "Cqc.h"
#include "CqcLogic.h"
#include "DogLogic.h"
#include "Fulton.h"
#include "Game.h"
#include "Host.h"
#include "Hud.h"
#include "InputBridge.h"
#include "Log.h"
#include "Marking.h"
#include "MarkingLogic.h"
#include "Mem.h"
#include "Puppet.h"
#include "RenderMarkers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <format>
#include <map>
#include <optional>

#include <windows.h>

namespace phantom::er::Dog
{
	namespace
	{
		constexpr std::uint16_t kLb = 0x0100;           // XINPUT_GAMEPAD_LEFT_SHOULDER: MGSV's call menu while held
		constexpr std::uint8_t  kRenderBit = 0x08;      // ChrIns+0x1C5 bit3 enable_render (er/Marking.cpp)
		constexpr std::uint8_t  kStareBits = 0x30;      // ChrIns+0x538: bit4 no attack, bit5 no move (the decoys' stare)
		constexpr ULONGLONG     kSpotMs = 250;          // the dog smells 4 times a second
		constexpr ULONGLONG     kPickMs = 250;          // the pick 4 times a second while LB is held (or just after)...
		constexpr ULONGLONG     kPickIdleMs = 1000;     // ...once a second otherwise
		constexpr ULONGLONG     kLbAfterMs = 2000;      // "just after": an order arrives a few frames after the click
		constexpr ULONGLONG     kLookHintMs = 600;      // LB held this long with no pick: "look at an enemy"
		constexpr ULONGLONG     kConnectDedupeMs = 1000;
		constexpr ULONGLONG     kEngagedGraceMs = 1500;  // kBuddyEngaged must show this soon after Connected
		constexpr std::uint32_t kPhysHitHeight = 0x2F0;  // CSChrPhysicsModule hit capsule (er/Marking.cpp, er/Combat.cpp)
		constexpr std::uint32_t kPhysHitRadius = 0x2F4;
		constexpr std::uint32_t kPhysChrHitHeight = 0x2E0;
		constexpr std::uint32_t kPhysChrHitRadius = 0x2E4;
		constexpr float         kPaw[4]{ 1.0f, 0.78f, 0.25f, 1.0f };  // the paw: MGSV's buddy amber

		struct Chr
		{
			game::Character                c;
			bool                           hostile{ false };  // both team reads say so (StrictHostile)
			bool                           rendered{ true };
			std::optional<combat::Capsule> body;
		};

		struct Engaged
		{
			std::uint64_t   handle{ 0 };
			proto::DogStyle style{ proto::kDogBusy };
			dog::Act        act{ dog::Act::kNone };
			ULONGLONG       at{ 0 };
			ULONGLONG       nextFlinch{ 0 };
			std::int32_t    maxHp{ 0 };
			Vec3            dir;
			std::string     name;
			bool            followFlag{ true };  // ends when the guest's kBuddyEngaged is off (not for `dog attack`)
		};

		struct QueuedHit
		{
			std::uint64_t     handle{ 0 };
			combat::HitEffect effect;
			Vec3              dir;
			ULONGLONG         asked{ 0 };
			bool              flinch{ false };  // a busy hold's repeat flinch (dropped when the hold ends)
			std::string       what;
		};

		struct Starer
		{
			std::uint8_t* chr{ nullptr };
			std::uint64_t handle{ 0 };
			std::uint8_t  setBits{ 0 };
			ULONGLONG     until{ 0 };
			std::string   name;
		};

		std::atomic<std::uint64_t> g_target{ 0 };
		std::string                g_targetName;
		dog::PickResult            g_last;
		dog::PickLatch             g_latch;  // the shown pick kept through the click (er/DogLogic.h PickLatch)
		std::string                g_freshWhy;  // what the rules said last while the latch kept the pick
		ULONGLONG                  g_pawAt{ 0 };  // the paw was last drawn then...
		std::uint64_t              g_pawHandle{ 0 };  // ...over him
		std::string                g_lastWhy{ "not run yet" };
		ULONGLONG                  g_lastPick{ 0 }, g_lastSpot{ 0 };
		dog::Spotter               g_spotter;
		dog::FoundText             g_found;
		bool                       g_lbDown{ false };
		ULONGLONG                  g_lbDownAt{ 0 }, g_lbUpAt{ 0 };
		bool                       g_lookSaid{ false };
		bool                       g_pointedAttack{ false };  // this LB hold pointed the right stick at the attack entry
		std::optional<proto::DogStyle> g_style;  // the dog's style as the guest last said (kGEvBuddyOrder pad[0], kGEvDogAttack)
		std::uint32_t              g_lastCommand{ 0 };
		ULONGLONG                  g_lastCommandAt{ 0 };
		std::optional<Engaged>     g_engaged;
		std::deque<QueuedHit>      g_hits;
		std::vector<Starer>        g_starers;
		std::map<std::uint64_t, ULONGLONG> g_barkCooldown;
		std::map<std::uint64_t, ULONGLONG> g_lastConnect;
		cqc::Cooldowns             g_bossStagger;
		bool                       g_testBoss{ false };
		bool                       g_wasLive{ false };
		bool                       g_drawn{ false };
		std::array<std::uint64_t, 5> g_phases{};
		std::array<std::uint64_t, static_cast<std::size_t>(dog::Act::kCount)> g_acts{};
		std::uint64_t g_notHostile{ 0 }, g_gone{ 0 }, g_spotted{ 0 }, g_texts{ 0 }, g_barks{ 0 }, g_stares{ 0 }, g_pickChanges{ 0 }, g_picks{ 0 },
			g_rays{ 0 }, g_hitsFired{ 0 }, g_hitsLost{ 0 }, g_hitsLate{ 0 }, g_flinches{ 0 }, g_deduped{ 0 }, g_lookHints{ 0 }, g_taken{ 0 },
			g_notLive{ 0 }, g_quietEnds{ 0 }, g_latchHolds{ 0 }, g_latchKept{ 0 }, g_clickPicked{ 0 }, g_clickLatched{ 0 }, g_clickNone{ 0 },
			g_clickPawNone{ 0 }, g_clickRevived{ 0 };
		bool g_fromConsole{ false };  // `dog attack`: no guest behind it, so no kBuddyEngaged to follow

		Vec3 V(const float* a_v) { return { a_v[0], a_v[1], a_v[2] }; }

		dog::Rules Rules() { return dog::MakeRules(Cfg().dog); }

		bool Live(const Frame& a_f)
		{
			const auto& g = a_f.guest;
			return Cfg().dog.enabled && Puppet::Active() && a_f.player && a_f.guestReady && a_f.inGame && !a_f.loading && !a_f.dead && !a_f.dying &&
			       !a_f.cutscene && g.buddyType == Buddy::kDog && (g.buddyFlags & proto::kBuddyPresent) && !Buddy::Absent();
		}

		// Never a friend: the ActorTable's kActorHostile rule (an EnemyIns of another team the team relation table makes the
		// Tarnished's enemy, by his raw team byte) AND the same by the team GetTeamType gives (every hit's read). A merchant, a quest
		// character, a spirit summon, a neutral NPC fail both.
		bool StrictHostile(const Frame& a_f, const game::Character& a_c, std::uint8_t a_myTeam)
		{
			return a_c.isEnemy && !a_c.isPlayer && a_c.chr != a_f.player.chr && a_c.team != a_myTeam && Combat::HostileToPlayer(a_f, a_c.team) &&
			       Combat::HostileChr(a_f, a_c.chr);
		}

		bool SaneCapsule(float a_h, float a_r) { return std::isfinite(a_h) && std::isfinite(a_r) && a_r >= 0.08f && a_r <= 10.0f && a_h >= 0.2f && a_h <= 40.0f; }

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

		std::vector<Chr> Around(const Frame& a_f, bool a_bodies)
		{
			std::vector<Chr> out;
			const auto       myTeam = SafeRead<std::uint8_t>(a_f.player.chr + game::kChrTeam).value_or(0);
			for (const auto& c : game::Characters(65)) {
				if (c.isPlayer || c.chr == a_f.player.chr || !Finite(c.pos)) {
					continue;
				}
				Chr x{ c };
				x.hostile = StrictHostile(a_f, c, myTeam);
				x.rendered = (SafeRead<std::uint8_t>(c.chr + game::kChrFlags1C5).value_or(kRenderBit) & kRenderBit) != 0 && c.maxHp > 0;
				if (a_bodies) {
					x.body = Body(c);
				}
				out.push_back(std::move(x));
			}
			return out;
		}

		std::string NameOf(const game::Character& a_c) { return std::format("npc {} team {} hp {}/{} ({:#x})", a_c.npcParam, a_c.team, a_c.hp, a_c.maxHp, a_c.handle); }

		Vec3 Row(const std::uint8_t* a_cam, int a_row)
		{
			const auto* f = reinterpret_cast<const float*>(a_cam + game::kCamMatrix + 16 * a_row);
			return { f[0], f[1], f[2] };
		}

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

		// Where the dog is in Elden Ring: its placement (er/Buddy.cpp: the guest's pose, ground-snapped), else Snake's feet.
		Vec3 DogFeet(const Frame& a_f, bool* a_placed = nullptr)
		{
			const bool placed = Buddy::HasEstimate() && Finite(Buddy::Position());
			if (a_placed) {
				*a_placed = placed;
			}
			return placed ? Buddy::Position() : game::Position(a_f.player);
		}

		void SetTarget(std::uint64_t a_handle, const std::string& a_name)
		{
			if (a_handle != g_target.load()) {
				++g_pickChanges;
				Log::Info("dog: the pick (kActorDogTarget) is {}", a_handle ? a_name : std::format("nobody ({})", g_lastWhy));
			}
			g_target = a_handle;
			g_targetName = a_handle ? a_name : std::string();
		}

		bool Fetching() { return g_style && *g_style == proto::kDogFetch; }

		std::vector<dog::Candidate> Candidates(const std::vector<Chr>& a_chrs)
		{
			std::vector<dog::Candidate> out;
			out.reserve(a_chrs.size());
			for (const auto& x : a_chrs) {
				dog::Candidate c;
				c.handle = x.c.handle;
				c.feet = x.c.pos;
				c.aim = x.c.pos + Vec3{ 0, marking::AimHeight(x.body, x.c.pos), 0 };
				c.radius = x.body ? x.body->r : 0.5f;
				c.hostile = x.hostile;
				c.alive = !x.c.dead && x.c.hp > 0;
				c.rendered = x.rendered;
				if (x.hostile && c.alive) {
					c.boss = g_testBoss || Cqc::IsBoss(x.c.npcParam, x.c.maxHp);
					c.held = Cqc::IsHeld(x.c.handle);
					c.lifting = Fulton::IsLifting(x.c.handle);
					c.asleep = Combat::Asleep(x.c.chr);
					c.marked = Marking::IsMarked(x.c.handle);
				}
				out.push_back(c);
			}
			return out;
		}

		// The dog's run from a_dog to a_c (er/DogLogic.h ClearRun) with Elden Ring's rays: down-rays along the line, a knee-high ray.
		dog::Run RunOf(const Vec3& a_dog, const dog::Candidate& a_c, const std::uint8_t* a_ignore, float a_runStepMax)
		{
			const auto pts = dog::RunPoints(a_dog, a_c.feet, a_c.radius + 0.3f);
			std::vector<std::optional<float>> ground;
			ground.reserve(pts.size());
			const float len = std::max(1e-3f, LengthXZ(a_c.feet - a_dog));
			for (const auto& p : pts) {
				// The line's height there: from the dog's feet to his.
				const float t = std::clamp(LengthXZ(p - a_dog) / len, 0.0f, 1.0f);
				const float lineY = a_dog.y + (a_c.feet.y - a_dog.y) * t;
				Vec3        hit;
				++g_rays;
				// Snake's ground filter (floors only characters stand on count; an enemy's capsule doesn't).
				if (Puppet::CastWorld(Vec3{ p.x, lineY + dog::kRunAbove, p.z }, Vec3{ 0, -(dog::kRunAbove + dog::kRunBelow), 0 }, hit, a_ignore, true)) {
					ground.push_back(hit.y);
				} else {
					ground.push_back(std::nullopt);
					break;  // a drop: no need to look further
				}
			}
			bool wall = false;
			if (ground.size() == pts.size() && ground.size() >= 2 && ground.front() && ground.back()) {
				const Vec3 from{ pts.front().x, *ground.front() + dog::kRunKnee, pts.front().z };
				const Vec3 to{ pts.back().x, *ground.back() + dog::kRunKnee, pts.back().z };
				Vec3       hit;
				++g_rays;
				wall = Puppet::CastWorld(from, to - from, hit, a_ignore, true) && Length(hit - to) > 0.3f;
			}
			return dog::ClearRun(ground, wall, a_runStepMax);
		}

		// The latched (or paw-shown) one may stay or become the pick again: the hard rules (wider cone) and, with clear_run, a clear
		// run from the dog -- the latch kept a target the rules had just refused for a wall or a drop (review 0.8.0). Sight not re-asked.
		bool LatchKeepsHim(const Frame& a_f, const dog::Candidate& a_c, const marking::Camera& a_cam, const dog::Rules& a_rules)
		{
			const Vec3       dog = DogFeet(a_f);
			const auto*      ignore = a_f.player.chr;
			const bool       rays = game::CanCastRay();
			const dog::RunFn run = [&](const dog::Candidate& a_x) { return rays ? RunOf(dog, a_x, ignore, a_rules.runStepMax) : dog::Run::kClear; };
			return dog::LatchKeeps(a_c, { a_cam.pos, a_cam.fwd }, game::Position(a_f.player), Fetching(), a_rules, run);
		}

		// The pick (er/DogLogic.h Pick) with Elden Ring's rays: the camera's sight line, the dog's run.
		dog::PickResult RunPick(const Frame& a_f, const std::vector<Chr>& a_chrs, std::vector<dog::Candidate>& a_cands, bool a_log)
		{
			a_cands = Candidates(a_chrs);
			marking::Camera cam;
			if (!ReadCamera(cam)) {
				g_lastWhy = "no camera";
				return {};
			}
			const auto  rules = Rules();
			const Vec3  snake = game::Position(a_f.player);
			const Vec3  dog = DogFeet(a_f);
			const bool  rays = game::CanCastRay();
			const auto* ignore = a_f.player.chr;
			const auto  bodyOf = [&](const dog::Candidate& a_c) -> std::optional<combat::Capsule> {
				for (const auto& x : a_chrs) {
					if (x.c.handle == a_c.handle) {
						return x.body;
					}
				}
				return std::nullopt;
			};
			const dog::SightFn sight = [&](const dog::Candidate& a_c) {
				if (!rays) {
					return true;
				}
				++g_rays;
				Vec3 hit;
				if (!game::CastRay(cam.pos, a_c.aim - cam.pos, hit, ignore, Cfg().rayFilter)) {
					return true;
				}
				return marking::RayClear(cam.pos, a_c.aim, hit, bodyOf(a_c));
			};
			const dog::RunFn run = [&](const dog::Candidate& a_c) { return rays ? RunOf(dog, a_c, ignore, rules.runStepMax) : dog::Run::kClear; };
			auto res = dog::Pick(a_cands, { cam.pos, cam.fwd }, snake, Fetching(), rules, sight, run);
			++g_picks;
			if (res.index) {
				g_lastWhy = "picked";
			} else if (res.nearest && *res.nearest < a_chrs.size()) {
				g_lastWhy = std::format("{} -- {}{}", NameOf(a_chrs[*res.nearest].c), dog::Name(res.why),
					res.why == dog::Why::kNoRun ? std::format(" ({})", dog::Name(res.run)) : std::string());
			} else {
				g_lastWhy = dog::Name(res.why);
			}
			if (a_log && !res.index) {
				Log::Info("dog: no pick: {}", g_lastWhy);
			}
			return res;
		}

		// ---- hits ----------------------------------------------------------------------------------------------------------------
		void Enqueue(std::uint64_t a_handle, const combat::HitEffect& a_e, const Vec3& a_dir, bool a_flinch, std::string a_what)
		{
			if (g_hits.size() >= 4) {
				++g_hitsLost;
				Log::Info("dog: {} dropped (four hits already wait for the bullet row)", g_hits.front().what);
				g_hits.pop_front();
			}
			g_hits.push_back({ a_handle, a_e, a_dir, ::GetTickCount64(), a_flinch, std::move(a_what) });
		}

		// One waiting hit when the bullet row is free (CQC's rule, cqc::WhenToFire), on him only if he is still a living hostile.
		void FireHits(const Frame& a_f, const std::vector<Chr>& a_chrs)
		{
			if (g_hits.empty() || a_chrs.empty()) {
				return;  // (no characters read this frame: the next one reads them, a hit is queued)
			}
			const auto& c = Cfg();
			const auto  now = ::GetTickCount64();
			auto&       q = g_hits.front();
			const auto  when = cqc::WhenToFire(now, q.asked, static_cast<std::uint32_t>(std::max(0, c.cqcSettleMs)),
				 static_cast<std::uint32_t>(std::max(0, c.cqcRetryMs)), Combat::RowBusy());
			if (when == cqc::Wait::kWait) {
				return;
			}
			if (when == cqc::Wait::kFireLate) {
				++g_hitsLate;
			}
			const Chr* x = nullptr;
			for (const auto& y : a_chrs) {
				if (y.c.handle == q.handle) {
					x = &y;
					break;
				}
			}
			if (!x || !x->hostile || x->c.dead || x->c.hp <= 0 || Fulton::IsLifting(q.handle)) {
				// (A balloon may have taken him during the hit's settle: never a hit on one going up.)
				++g_hitsLost;
				Log::Info("dog: {} not landed -- {}", q.what,
					!x                            ? "he is gone"
					: !x->hostile                 ? "NOT a hostile enemy (never hit)"
					: Fulton::IsLifting(q.handle) ? "a Fulton balloon has him"
					                              : "dead already");
				g_hits.pop_front();
				return;
			}
			// From the dog's side of him, at his chest: a raised shield faces the dog (v1), and Combat's reach test always finds him.
			const float r = x->body ? x->body->r : 0.5f;
			Vec3        dir{ q.dir.x, 0, q.dir.z };
			dir = LengthXZ(dir) > 1e-3f ? Normalize(dir) : Vec3{ 0, 0, 1 };
			const Vec3 from = x->c.pos + Vec3{ 0, 1.0f, 0 } - dir * (r + 0.8f);
			if (Combat::HitCharacter(a_f, q.handle, from, r + 1.5f, q.effect, q.what)) {
				++g_hitsFired;
				g_flinches += q.flinch ? 1 : 0;
			} else {
				++g_hitsLost;
				Log::Info("dog: {} -- out of reach (or [Combat] damage is off)", q.what);
			}
			g_hits.pop_front();
		}

		void DropFlinches(std::uint64_t a_handle)
		{
			std::erase_if(g_hits, [&](const QueuedHit& h) { return h.flinch && h.handle == a_handle; });
		}

		void EndEngagement(const char* a_why)
		{
			if (!g_engaged) {
				return;
			}
			Cqc::LetGo(g_engaged->handle, a_why);
			DropFlinches(g_engaged->handle);
			Log::Info("dog: done with {} ({}; {})", g_engaged->name, dog::Name(g_engaged->act), a_why);
			g_engaged.reset();
		}

		// ---- the bite ------------------------------------------------------------------------------------------------------------
		void Connect(const Frame& a_f, std::uint64_t a_handle, proto::DogStyle a_style, float a_share, const Vec3& a_dir)
		{
			const auto now = ::GetTickCount64();
			if (auto it = g_lastConnect.find(a_handle); it != g_lastConnect.end() && now - it->second < kConnectDedupeMs) {
				++g_deduped;
				Log::Info("dog: a second Connected on {:#x} within {} ms -- one effect per bite", a_handle, kConnectDedupeMs);
				return;
			}
			const auto chrs = Around(a_f, true);
			const Chr* x = nullptr;
			for (const auto& y : chrs) {
				if (y.c.handle == a_handle) {
					x = &y;
					break;
				}
			}
			if (!x) {
				++g_gone;
				Log::Info("dog: connected on ER {:#x}, who isn't in Elden Ring's list (gone, fultoned) -- nothing done", a_handle);
				return;
			}
			if (!x->hostile) {
				// The safety rule: only a hostile Elden Ring enemy is ever bitten (the host's pick is one; a guest's own choice or a
				// stale handle may not be).
				++g_notHostile;
				Log::Warn("dog: connected on {} -- NOT a hostile Elden Ring enemy (a friend, a neutral, a merchant, a summon): nothing done", NameOf(x->c));
				return;
			}
			if (x->c.dead || x->c.hp <= 0) {
				Log::Info("dog: connected on {} -- dead already: nothing done", NameOf(x->c));
				return;
			}
			// Never one a Fulton balloon lifts (a kill would end him on it, and Elden Ring would pay his runes as the Fulton does) nor
			// one Snake's CQC holds (review 0.8.0): the guest calls a fetch's yank Connected with the order's style, and Snake may have
			// fultoned or grabbed the very enemy the dog was sent at.
			if (Fulton::IsLifting(a_handle)) {
				++g_taken;
				Log::Info("dog: connected on {} -- a Fulton balloon has him: nothing done", NameOf(x->c));
				return;
			}
			if (Cqc::IsHeld(a_handle) && !Cqc::DogHolds(a_handle)) {
				++g_taken;
				Log::Info("dog: connected on {} -- Snake holds him (CQC): nothing done", NameOf(x->c));
				return;
			}
			g_lastConnect[a_handle] = now;
			const auto rules = Rules();
			const bool boss = g_testBoss || Cqc::IsBoss(x->c.npcParam, x->c.maxHp);
			const auto name = NameOf(x->c);
			bool       holdRefused = true;
			if (a_style == proto::kDogBusy && !boss) {
				holdRefused = !Cqc::HoldFor(a_handle, rules.busyHold, name);
			}
			const auto eff = dog::EffectOf(a_style, x->c.maxHp, a_share, boss, holdRefused, g_bossStagger.Cooling(a_handle, now), rules, Combat::CurrentRules(),
				Cqc::CurrentRules(), Combat::CurrentMelee());
			++g_acts[static_cast<std::size_t>(eff.act)];
			const auto what = std::format("D-Dog {}", dog::Name(eff.act));
			Log::Info("dog: bit {} ({}) -> {}: damage {}{}, poise {:.0f} lv {}, sleep {} / {:.2f} of his resistance{}{}", name, dog::StyleName(a_style),
				dog::Name(eff.act), eff.hit.damage, eff.hit.nonLethal ? " (never below 1 HP)" : "", eff.hit.poise, eff.hit.dmgLevel, eff.hit.sleep,
				eff.hit.sleepShare, eff.holdSeconds > 0 ? std::format(", held {:.1f} s", eff.holdSeconds) : std::string(),
				a_share > 0 ? std::format(" (MGSV's share {:.3f})", a_share) : std::string());
			switch (eff.act) {
			case dog::Act::kHold:
			case dog::Act::kFlinch:
			case dog::Act::kKill:
			case dog::Act::kKnockOut:
			case dog::Act::kWound:
				if (g_engaged && g_engaged->handle == a_handle && eff.act == dog::Act::kHold) {
					// Bitten again while the dog holds him: HoldFor just refreshed that hold -- keep it (review 0.8.0: letting go here
					// released him at once and put him on hold cooldown).
					DropFlinches(a_handle);
					g_engaged.reset();
				} else {
					EndEngagement("D-Dog took another one");
				}
				Enqueue(a_handle, eff.hit, a_dir, false, what);
				g_engaged = Engaged{ a_handle, a_style, eff.act, now, now + static_cast<ULONGLONG>(std::max(0.2f, rules.busyFlinchEvery) * 1000.0f), x->c.maxHp,
					a_dir, name, !g_fromConsole };
				break;
			case dog::Act::kStagger:
				Enqueue(a_handle, eff.hit, a_dir, false, what);
				g_bossStagger.Start(a_handle, now, dog::kBossStaggerCooldown);
				break;
			case dog::Act::kBossRefused:
				Log::Warn("dog: {} is a boss ([Dog] bosses = off) -- the dog's bite does nothing to him", name);
				break;
			case dog::Act::kFetch:
				Log::Info("dog: Get it on {} -- MGSV's Fulton follows (kGEvFulton)", name);
				break;
			default:
				break;
			}
		}

		// ---- Bark ----------------------------------------------------------------------------------------------------------------
		void ReleaseStarers(const char* a_why, bool a_all, ULONGLONG a_now)
		{
			if (g_starers.empty()) {
				return;
			}
			const auto  chars = game::Characters(256, true);
			std::size_t n = 0;
			std::erase_if(g_starers, [&](const Starer& s) {
				const game::Character* c = nullptr;
				for (const auto& x : chars) {
					if (x.handle == s.handle && x.chr == s.chr) {
						c = &x;
						break;
					}
				}
				if (!a_all && c && !c->dead && !c->disabled && a_now < s.until) {
					return false;
				}
				// Our bits back (a dead one too: Elden Ring revives corpses in place at a rest); a Fulton's lift keeps its own.
				if (c && !c->disabled && !Fulton::IsLifting(s.handle)) {
					auto*      flags = s.chr + game::kChrDebugFlags;
					const auto v = SafeRead<std::uint8_t>(flags).value_or(0);
					SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(v & ~s.setBits));
				}
				++n;
				return true;
			});
			if (n) {
				Log::Info("dog: {} bark starer(s) go on ({})", n, a_why);
			}
		}

		void Bark(const Frame& a_f, const char* a_who)
		{
			++g_barks;
			const auto  rules = Rules();
			const auto  now = ::GetTickCount64();
			bool        placed = false;
			const Vec3  dog = DogFeet(a_f, &placed);
			const Vec3  chest = dog + Vec3{ 0, 0.5f, 0 };
			const Vec3  snake = game::Position(a_f.player) + Vec3{ 0, 1.0f, 0 };
			const auto  chrs = Around(a_f, false);
			std::size_t heard = 0, around = 0;
			for (const auto& foe : Combat::HostilesNear(a_f, chest, rules.barkRange + 2.0f)) {
				const Chr* x = nullptr;
				for (const auto& y : chrs) {
					if (y.c.handle == foe.handle) {
						x = &y;
						break;
					}
				}
				++around;
				dog::Listener l;
				l.eye = foe.cap.b;
				l.forward = Rotate(foe.orientation, { 0, 0, 1 }) * static_cast<float>(Calibration::ErForward());
				l.toSnake = combat::CapsuleDistance(foe.cap, snake);
				l.hostile = x && x->hostile && x->rendered;
				l.asleep = Combat::Asleep(foe.chr);
				l.boss = Cqc::IsBoss(foe.npcParam, foe.maxHp);
				l.held = Cqc::IsHeld(foe.handle);
				l.lifting = Fulton::IsLifting(foe.handle);
				if (!dog::HearsBark(l, chest, rules)) {
					continue;
				}
				if (std::ranges::any_of(g_starers, [&](const Starer& s) { return s.handle == foe.handle; })) {
					continue;
				}
				if (auto it = g_barkCooldown.find(foe.handle); it != g_barkCooldown.end() && now < it->second) {
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
				s.until = now + static_cast<ULONGLONG>(std::max(0.5f, rules.barkStare) * 1000.0f);
				s.name = foe.name;
				SafeWrite<std::uint8_t>(flags, static_cast<std::uint8_t>(*v | kStareBits));
				g_barkCooldown[foe.handle] = s.until + static_cast<ULONGLONG>(std::max(0.0f, rules.barkCooldown) * 1000.0f);
				g_starers.push_back(s);
				++heard;
				++g_stares;
				Log::Info("dog: {} hears D-Dog bark {:.1f} m away and stops to stare ({:.0f} s)", foe.name, Length(foe.cap.b - chest), rules.barkStare);
			}
			Log::Info("dog: Bark ({}) at {} ({:.1f}, {:.1f}, {:.1f}): {} of {} hostile(s) within {:.0f} m stare", a_who, placed ? "the dog" : "Snake (the dog isn't placed)",
				dog.x, dog.y, dog.z, heard, around, rules.barkRange);
		}

		void Draw(const Frame& a_f, const std::vector<Chr>& a_chrs, bool a_lb)
		{
			std::vector<Marker> out;
			const auto          pick = g_target.load();
			marking::Camera     cam;
			if (Cfg().dog.indicator && a_lb && pick && !(a_f.guest.flags & proto::kGuestInMenu) && !a_f.menuOpen && ReadCamera(cam)) {
				for (const auto& x : a_chrs) {
					if (x.c.handle != pick) {
						continue;
					}
					marking::Style style;
					style.size = Cfg().marking.size;
					const float height = marking::MarkHeight(x.body, x.c.pos, Cfg().marking.height, Cfg().marking.above);
					marking::BuildPaw(out, cam, x.c.pos + Vec3{ 0, height, 0 }, kPaw, 1.0f, style);
					g_pawAt = ::GetTickCount64();
					g_pawHandle = pick;
					break;
				}
			}
			if (out.empty() && !g_drawn) {
				return;
			}
			g_drawn = !out.empty();
			RenderMarkers::Set(RenderMarkers::Owner::kDog, std::move(out));
		}

		void Stop(const char* a_why)
		{
			EndEngagement(a_why);
			g_hits.clear();
			ReleaseStarers(a_why, true, ::GetTickCount64());
			if (g_target.load()) {
				SetTarget(0, {});
			}
			g_latch.Clear();
			if (g_drawn) {
				RenderMarkers::Set(RenderMarkers::Owner::kDog, {});
				g_drawn = false;
			}
			g_spotter.Clear();
		}

		proto::DogStyle ResolveStyle(std::uint32_t a_raw, const char** a_from)
		{
			if (a_raw <= proto::kDogWound) {
				*a_from = "the guest";
				return static_cast<proto::DogStyle>(a_raw);
			}
			// The guest couldn't read it: the label the player clicked (the call menu names the equipped attack), else what the
			// guest said before, else the gentlest (busy).
			if (::GetTickCount64() - g_lastCommandAt < 30000) {
				if (const auto s = dog::StyleOfCommand(g_lastCommand)) {
					*a_from = "the player's order";
					return *s;
				}
			}
			if (g_style) {
				*a_from = "learned before";
				return *g_style;
			}
			*a_from = "unknown: busy";
			return proto::kDogBusy;
		}
	}

	void OnAttack(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		const auto          phase = static_cast<std::uint32_t>(proto::DogAttackPhase(a_e.a));
		const auto          raw = static_cast<std::uint32_t>(proto::DogAttackStyle(a_e.a));
		const std::uint64_t handle = a_e.pad[0] | (static_cast<std::uint64_t>(a_e.pad[1]) << 32);
		const float         share = static_cast<float>(a_e.b) / 10000.0f;
		if (phase < g_phases.size()) {
			++g_phases[phase];
		}
		if (!Cfg().dog.enabled) {
			Log::Info("dog: kGEvDogAttack {} style {} (MGSV attack {}, share {:.4f}) on ER {:#x}, the dog at {:.2f},{:.2f},{:.2f} -- [Dog] enabled = false: logged only",
				dog::PhaseName(phase), dog::StyleName(raw), proto::DogAttackId(a_e.a), share, handle, a_e.v0[0], a_e.v0[1], a_e.v0[2]);
			return;
		}
		const char* from = "";
		const auto  style = ResolveStyle(raw, &from);
		if (raw <= proto::kDogWound) {
			g_style = style;
		}
		Log::Info("dog: kGEvDogAttack {} style {} ({}) (MGSV attack {}, share {:.4f}) on ER {:#x}{}", dog::PhaseName(phase), dog::StyleName(style), from,
			proto::DogAttackId(a_e.a), share, handle, handle && handle == g_target.load() ? " -- the host's pick" : "");
		if (!Puppet::Active() || !a_f.player) {
			return;
		}
		Vec3 dir{};
		if (Finite(V(a_e.v1)) && Length(V(a_e.v1)) > 1e-3f) {
			dir = Puppet::CurrentAnchor().DirToEr(V(a_e.v1));
		}
		switch (phase) {
		case proto::kDogOrdered:
			break;
		case proto::kDogConnected:
			if (handle && !Live(a_f)) {
				// Snake dying, a cutscene, a load, the dog not out (review 0.8.0): nothing queued that Tick wouldn't fire or release.
				++g_notLive;
				Log::Info("dog: connected on ER {:#x} while D-Dog isn't live here (Snake not playing, a load, the dog not out) -- nothing done", handle);
			} else if (handle) {
				if (LengthXZ(dir) < 1e-3f) {
					// No direction from the guest: from the dog's place toward him.
					for (const auto& c : game::Characters(128)) {
						if (c.handle == handle) {
							dir = c.pos - DogFeet(a_f);
							break;
						}
					}
				}
				Connect(a_f, handle, style, share, dir);
			}
			break;
		case proto::kDogReleased:
			if (g_engaged && (!handle || g_engaged->handle == handle)) {
				EndEngagement("D-Dog let go (MGSV)");
			}
			break;
		case proto::kDogRefused:
			// No target: why the enemy in view wasn't the host's pick, when there was one near the centre. Not after the player's own
			// whistle / Wait / DD ended the order, nor while D-Dog isn't live (a load, Snake dying): nothing failed then (review 0.8.0).
			if (dog::QuietEnd(g_lastCommand, ::GetTickCount64() - g_lastCommandAt, Live(a_f))) {
				++g_quietEnds;
				Log::Info("dog: {} after the player's own order {} -- nothing to say", dog::PhaseName(phase), g_lastCommand);
			} else {
				Hud::Notify(handle ? std::string("D-Dog can't take that one") : g_last.nearest ? dog::NoPickText(g_last, Rules()) : std::string("D-Dog: no enemy in sight"),
					Hud::Tone::kWarn);
			}
			break;
		case proto::kDogLost:
			if (dog::QuietEnd(g_lastCommand, ::GetTickCount64() - g_lastCommandAt, Live(a_f))) {
				++g_quietEnds;
				Log::Info("dog: {} after the player's own order {} -- nothing to say", dog::PhaseName(phase), g_lastCommand);
			} else {
				Hud::Notify("D-Dog couldn't reach him", Hud::Tone::kWarn);
			}
			if (g_engaged && (!handle || g_engaged->handle == handle)) {
				EndEngagement("lost");
			}
			break;
		default:
			Log::Warn("dog: kGEvDogAttack phase {} unknown", phase);
			break;
		}
	}

	void OnOrder(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (a_e.b != Buddy::kDog) {
			return;
		}
		g_lastCommand = a_e.a;
		g_lastCommandAt = ::GetTickCount64();
		if (a_e.pad[0] >= 1 && a_e.pad[0] - 1 <= proto::kDogWound) {
			const auto s = static_cast<proto::DogStyle>(a_e.pad[0] - 1);
			if (!g_style || *g_style != s) {
				Log::Info("dog: D-Dog's style is {} (its equipment, from the guest)", dog::StyleName(s));
			}
			g_style = s;
		}
		if (!Cfg().dog.enabled || !Live(a_f)) {
			return;
		}
		if (a_e.a >= 11 && a_e.a <= 15) {
			g_lastPick = 0;  // the pick again this frame, with the style just learned (Get it takes sleepers only)
			// What the guest finds at the click (kActorDogTarget as published now): the measure of the latch (2 of 16 orders refused
			// "out of sight" in the 0.8.0 run although the paw had shown).
			const auto now = ::GetTickCount64();
			// The click a moment after the paw went (the latch ran out as the player let the stick go): the one it showed over is the
			// pick again if he still fits -- the guest looks for the pick again for 350 ms after the click.
			if (!g_target.load() && g_pawHandle && g_pawAt && now - g_pawAt < dog::kClickGraceMs && Cfg().dog.pickLatchMs > 0) {
				const auto      chrs = Around(a_f, true);
				const auto      cands = Candidates(chrs);
				marking::Camera cam;
				for (std::size_t i = 0; i < chrs.size() && i < cands.size(); ++i) {
					if (chrs[i].c.handle == g_pawHandle && ReadCamera(cam) && LatchKeepsHim(a_f, cands[i], cam, Rules())) {
						++g_clickRevived;
						g_latch.Revive(g_pawHandle, now);
						g_freshWhy = std::format("nobody ({}); the paw showed over him {} ms before the click", g_lastWhy, now - g_pawAt);
						SetTarget(g_pawHandle, NameOf(chrs[i].c));
						break;
					}
				}
			}
			const auto pick = g_target.load();
			const bool pawRecent = g_pawAt && now - g_pawAt < 1500;
			(pick ? (g_latch.Held() ? g_clickLatched : g_clickPicked) : g_clickNone) += 1;
			g_clickPawNone += !pick && pawRecent ? 1 : 0;
			Log::Info("dog: the player's attack order {}: the pick is {}{}; the paw {}", a_e.a, pick ? g_targetName : std::string("nobody"),
				g_latch.Held() ? std::format(" (kept by the latch: the rules say {})", g_freshWhy) : pick ? std::string() : std::format(" ({})", g_lastWhy),
				pawRecent ? std::format("showed over {} {} ms before", g_pawHandle == pick ? "him" : std::format("{:#x}", g_pawHandle), now - g_pawAt)
				          : std::string("didn't show in the last 1.5 s"));
		}
		if (a_e.a == 10 && Cfg().dog.bark) {
			Bark(a_f, "the player's order");
		}
	}

	void Tick(const Frame& a_f)
	{
		const auto now = ::GetTickCount64();
		g_bossStagger.Expire(now);
		std::erase_if(g_barkCooldown, [&](const auto& a_kv) { return now > a_kv.second; });
		std::erase_if(g_lastConnect, [&](const auto& a_kv) { return now - a_kv.second > 10000; });
		// (v17) The dog's style as the guest publishes it every frame (buddyFlags bits 8-15): the pick is made for it before any order
		// -- "Get it" picks sleepers from the start (review 0.8.0: learned only from the first order, the first fetch picked awake ones).
		if (a_f.guest.buddyType == Buddy::kDog) {
			if (const auto s = proto::BuddyDogStyle(a_f.guest.buddyFlags); s != proto::kDogStyleUnknown && (!g_style || *g_style != s)) {
				Log::Info("dog: D-Dog's style is {} (its equipment, published by the guest)", dog::StyleName(s));
				g_style = s;
				g_lastPick = 0;
			}
		}
		const bool live = Live(a_f);
		if (!live) {
			// Anything a Connected or a console bark left while not live is dropped and released here, not fired at the next live
			// frame on whoever bears that handle then (review 0.8.0).
			if (g_wasLive || !g_hits.empty() || g_engaged || !g_starers.empty()) {
				Stop(!Cfg().dog.enabled ? "[Dog] off" : "D-Dog not out / Snake not playing");
			}
			g_wasLive = false;
			return;
		}
		if (!g_wasLive) {
			Log::Info("dog: D-Dog is out and Snake plays -- spotting {}, the pick on, Bark {}", Cfg().dog.spot ? "on" : "off", Cfg().dog.bark ? "on" : "off");
		}
		g_wasLive = true;
		const auto& c = Cfg().dog;
		const auto  rules = Rules();
		// LB is the call menu's only while neither Elden Ring's nor MGSV's own menus (the iDroid pages its tabs with LB) have the pad.
		const bool  lb = (InputBridge::LastButtons() & kLb) != 0 && !a_f.menuOpen && !(a_f.guest.flags & (proto::kGuestInMenu | proto::kGuestOnVehicle));
		if (lb && !g_lbDown) {
			g_lbDownAt = now;
			g_lookSaid = false;
			g_pointedAttack = false;
		}
		// (0.8.0 polish) The why-no-pick line only once the player points at the attack entry: an LB hold for Wait or the whistle says
		// nothing.
		if (lb && dog::PointsAtAttack(InputBridge::LastRightStickX(), InputBridge::LastRightStickY())) {
			g_pointedAttack = true;
		}
		if (!lb && g_lbDown) {
			g_lbUpAt = now;
		}
		g_lbDown = lb;
		// Elden Ring's characters only when something needs them this frame (each read is a ReadProcessMemory): the 4 Hz spotting
		// and pick, the paw while LB is held, a hit waiting for the row; their capsules only for the pick and the paw.
		const bool lbRecent = lb || now - g_lbUpAt < kLbAfterMs;
		const bool spotDue = c.spot && now - g_lastSpot >= kSpotMs;
		const bool pickDue = now - g_lastPick >= (lbRecent ? kPickMs : kPickIdleMs);
		const bool pawDue = lb && c.indicator && g_target.load();
		const bool needChrs = spotDue || pickDue || pawDue || !g_hits.empty() || g_drawn;
		const auto chrs = needChrs ? Around(a_f, pickDue || pawDue || g_drawn) : std::vector<Chr>{};

		// Spotting: who the dog smells (provisional marks).
		if (spotDue) {
			g_lastSpot = now;
			std::vector<dog::Smelled> seen;
			seen.reserve(chrs.size());
			for (const auto& x : chrs) {
				seen.push_back({ x.c.handle, x.c.pos, x.hostile, !x.c.dead && x.c.hp > 0, x.rendered });
			}
			bool       placed = false;
			const Vec3 dog = DogFeet(a_f, &placed);
			const auto fresh = g_spotter.Update(seen, placed ? std::optional<Vec3>(dog) : std::nullopt, now, rules);
			for (const auto h : fresh) {
				++g_spotted;
				for (const auto& x : chrs) {
					if (x.c.handle == h) {
						Log::Info("dog: smells {} {:.1f} m from the dog", NameOf(x.c), LengthXZ(x.c.pos - dog));
						break;
					}
				}
			}
			for (const auto& [h, until] : g_spotter.All()) {
				Marking::Provisional(h, until);
			}
			if (const auto text = g_found.Update(fresh.size(), g_spotter.Count(now), now, rules.spotTextEvery); !text.empty()) {
				++g_texts;
				Hud::Notify(text, Hud::Tone::kInfo);
				Log::Info("dog: \"{}\"", text);
			}
		}

		// The pick (kActorDogTarget): 4 times a second around an LB hold, once a second otherwise.
		if (pickDue) {
			g_lastPick = now;
			std::vector<dog::Candidate> cands;
			g_last = RunPick(a_f, chrs, cands, false);
			const std::uint64_t fresh = g_last.index && *g_last.index < chrs.size() ? chrs[*g_last.index].c.handle : 0;
			// The shown pick stays through the click (DogLogic.h PickLatch): kept c.pickLatchMs after the rules stop choosing him, while
			// he still passes the hard rules (alive, hostile, not a boss, not held or lifted, in range...).
			bool        fits = false;
			std::size_t latched = chrs.size();
			if (const auto h = g_latch.Handle(); h && h != fresh) {
				marking::Camera cam;
				for (std::size_t i = 0; i < chrs.size() && i < cands.size(); ++i) {
					if (chrs[i].c.handle == h) {
						latched = i;
						fits = ReadCamera(cam) && LatchKeepsHim(a_f, cands[i], cam, rules);
						break;
					}
				}
			}
			const bool          wasHeld = g_latch.Held();
			const std::uint64_t heldHandle = g_latch.Handle();
			const ULONGLONG     freshBefore = g_latch.FreshAt();
			const auto          pick = g_latch.Update(fresh, fits, now, static_cast<std::uint64_t>(std::max(0, c.pickLatchMs)));
			if (g_latch.Held()) {
				++g_latchKept;
				const auto said = fresh ? std::format("{} instead", NameOf(chrs[*g_last.index].c)) : std::format("nobody ({})", g_lastWhy);
				if (!wasHeld) {
					++g_latchHolds;
					Log::Info("dog: the pick stays {} for up to {} ms (the rules now say {})", latched < chrs.size() ? NameOf(chrs[latched].c) : g_targetName,
						c.pickLatchMs, said);
				}
				g_freshWhy = said;
			} else if (wasHeld) {
				Log::Info("dog: the latch ends after {} ms: {}", now - freshBefore,
					pick && pick == heldHandle ? "the rules chose him again"
					: pick                     ? "another one took over"
					                           : "he no longer fits the hard rules, or the time is up");
			}
			if (pick && pick == fresh) {
				SetTarget(pick, NameOf(chrs[*g_last.index].c));
			} else if (pick && latched < chrs.size()) {
				SetTarget(pick, NameOf(chrs[latched].c));
			} else {
				SetTarget(0, {});
			}
		}
		if (lb && c.indicator && g_pointedAttack && !g_target.load() && !g_lookSaid && now - g_lbDownAt >= kLookHintMs) {
			// Once per hold: why the enemy in view isn't the dog's (a wall in his run, too close...), else "look at an enemy".
			g_lookSaid = true;
			++g_lookHints;
			const auto text = dog::NoPickText(g_last, rules);
			Hud::Notify(text, Hud::Tone::kInfo);
			Log::Info("dog: LB held, no pick: \"{}\" ({})", text, g_lastWhy);
		}

		// A busy hold: a flinch every busy_flinch_every s while the dog has him; the guest's kBuddyEngaged ends it too.
		if (g_engaged) {
			if (g_engaged->act == dog::Act::kHold && !Cqc::DogHolds(g_engaged->handle) && now - g_engaged->at > 300) {
				EndEngagement("the hold ended");
			} else if (g_engaged->act == dog::Act::kHold && now >= g_engaged->nextFlinch) {
				g_engaged->nextFlinch = now + static_cast<ULONGLONG>(std::max(0.2f, rules.busyFlinchEvery) * 1000.0f);
				Enqueue(g_engaged->handle, dog::Flinch(g_engaged->maxHp, Combat::CurrentRules(), Cqc::CurrentRules(), Combat::CurrentMelee()), g_engaged->dir, true,
					"D-Dog flinch");
			}
			if (g_engaged && g_engaged->followFlag && now - g_engaged->at > kEngagedGraceMs && !(a_f.guest.buddyFlags & proto::kBuddyEngaged)) {
				EndEngagement("the guest's kBuddyEngaged is off");
			}
		}
		FireHits(a_f, chrs);
		ReleaseStarers("bark_stare over", false, now);
		Draw(a_f, chrs, lb);
	}

	void OnPuppetStop() { Stop("the puppet stopped"); }

	std::uint64_t TargetHandle() { return g_target.load(); }

	std::string Describe()
	{
		const auto& c = Cfg().dog;
		const auto  now = ::GetTickCount64();
		const auto  a = [](dog::Act x) { return g_acts[static_cast<std::size_t>(x)]; };
		std::string s = std::format("dog: {} (spot {}, indicator {}, bark {}, order texts {}, bosses {}); style {}; pick {}{}; spotted {} now ({} this session, "
		                            "{} texts); kGEvDogAttack ordered {} connected {} released {} refused {} lost {}; effects: hold {} flinch {} kill {} "
		                            "knock out {} wound {} fetch {} stagger {} boss refused {}; not hostile {} (never touched), gone {}, deduped {}, on a balloon or "
		                            "in Snake's hold {}, not live {}; quiet ends {}; hits fired "
		                            "{} ({} flinches, {} late, {} lost), waiting {}; barks {}, stares {} ({} staring now); picks {} ({} changes, {} rays); the latch ({} ms): "
		                            "{} holds, {} picks kept; at the click: {} picked, {} by the latch ({} of them the paw's again), {} nobody ({} of them after the paw had "
		                            "shown); {}",
			c.enabled ? "ON" : "off ([Dog] enabled = false: kGEvDogAttack only logged)", c.spot ? "on" : "off", c.indicator ? "on" : "off", c.bark ? "on" : "off",
			c.orderText ? "on" : "off", c.bosses, g_style ? dog::StyleName(*g_style) : "unknown", g_target.load() ? g_targetName : std::string("nobody"),
			g_target.load() ? std::string() : std::format(" ({})", g_lastWhy), g_spotter.Count(now), g_spotted, g_texts, g_phases[0], g_phases[1], g_phases[2],
			g_phases[3], g_phases[4], a(dog::Act::kHold), a(dog::Act::kFlinch), a(dog::Act::kKill), a(dog::Act::kKnockOut), a(dog::Act::kWound),
			a(dog::Act::kFetch), a(dog::Act::kStagger), a(dog::Act::kBossRefused), g_notHostile, g_gone, g_deduped, g_taken, g_notLive, g_quietEnds, g_hitsFired, g_flinches, g_hitsLate,
			g_hitsLost, g_hits.size(), g_barks, g_stares, g_starers.size(), g_picks, g_pickChanges, g_rays, c.pickLatchMs, g_latchHolds, g_latchKept, g_clickPicked,
			g_clickLatched, g_clickRevived, g_clickNone, g_clickPawNone,
			g_engaged ? std::format("engaged: {} ({}, {:.1f} s)", g_engaged->name, dog::Name(g_engaged->act), (now - g_engaged->at) / 1000.0)
			          : std::string("not engaged"));
		return s;
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		auto& c = Cfg().dog;
		if (a_w.size() < 2 || a_w[1] == "status") {
			return Describe();
		}
		if (a_w[1] == "on" || a_w[1] == "off") {
			c.enabled = a_w[1] == "on";
			return Describe();
		}
		if (a_w[1] == "all" && a_w.size() > 2 && (a_w[2] == "on" || a_w[2] == "off")) {
			// The whole layer for a test run (not saved): enabled, spot, indicator, bark, order texts.
			c.enabled = c.spot = c.indicator = c.bark = c.orderText = a_w[2] == "on";
			return Describe();
		}
		if (a_w[1] == "set" && a_w.size() > 3) {
			const auto& k = a_w[2];
			const auto& v = a_w[3];
			const float f = std::strtof(v.c_str(), nullptr);
			const bool  b = v == "on" || v == "true" || v == "1";
			if (k == "enabled") c.enabled = b;
			else if (k == "spot") c.spot = b;
			else if (k == "indicator") c.indicator = b;
			else if (k == "bark") c.bark = b;
			else if (k == "order_text") c.orderText = b;
			else if (k == "target_marked_first") c.targetMarkedFirst = b;
			else if (k == "clear_run") c.clearRun = b;
			else if (k == "bosses") c.bosses = v == "stagger" ? "stagger" : "off";
			else if (k == "spot_range") c.spotRange = std::max(0.0f, f);
			else if (k == "spot_height") c.spotHeight = std::max(0.0f, f);
			else if (k == "spot_linger") c.spotLinger = std::max(0.0f, f);
			else if (k == "spot_text_every") c.spotTextEvery = std::max(0.0f, f);
			else if (k == "target_range") c.targetRange = std::max(0.0f, f);
			else if (k == "target_min") c.targetMin = std::max(0.0f, f);
			else if (k == "target_cone_deg") c.targetConeDeg = std::clamp(f, 0.5f, 90.0f);
			else if (k == "run_step_max") c.runStepMax = std::max(0.0f, f);
			else if (k == "busy_hold") c.busyHold = std::max(0.0f, f);
			else if (k == "busy_flinch_every") c.busyFlinchEvery = std::max(0.0f, f);
			else if (k == "kill_share_mult") c.killShareMult = std::max(0.0f, f);
			else if (k == "kill_share_fallback") c.killShareFallback = std::max(0.0f, f);
			else if (k == "wound_share") c.woundShare = std::max(0.0f, f);
			else if (k == "knockout_sleep") c.knockoutSleep = std::max(0.0f, f);
			else if (k == "bark_range") c.barkRange = std::max(0.0f, f);
			else if (k == "bark_stare") c.barkStare = std::max(0.0f, f);
			else if (k == "bark_cooldown") c.barkCooldown = std::max(0.0f, f);
			else if (k == "pick_latch_ms") c.pickLatchMs = std::clamp(static_cast<int>(f), 0, 5000);
			else if (k == "step_guard") c.stepGuard = b;
			else if (k == "wait_pin") c.waitPin = b;
			else {
				return "dog set <enabled|spot|indicator|bark|order_text|target_marked_first|clear_run|step_guard|wait_pin on|off | bosses off|stagger | spot_range|spot_height|spot_linger|"
				       "spot_text_every|target_range|target_min|target_cone_deg|run_step_max|busy_hold|busy_flinch_every|kill_share_mult|kill_share_fallback|"
				       "wound_share|knockout_sleep|bark_range|bark_stare|bark_cooldown|pick_latch_ms <value>>";
			}
			return Describe();
		}
		if (!a_f.player || !Puppet::Active()) {
			return "dog: needs the puppet running (Snake playing)";
		}
		if (a_w[1] == "style" && a_w.size() > 2) {
			static const std::map<std::string, proto::DogStyle> kStyles{ { "busy", proto::kDogBusy }, { "fetch", proto::kDogFetch }, { "kill", proto::kDogKill },
				{ "knockout", proto::kDogKnockOut }, { "wound", proto::kDogWound } };
			if (auto it = kStyles.find(a_w[2]); it != kStyles.end()) {
				g_style = it->second;
				g_lastPick = 0;
				return std::format("dog: style {} (as if the guest had said so)", dog::StyleName(it->second));
			}
			return "dog style busy|fetch|kill|knockout|wound";
		}
		if (a_w[1] == "pick" || (a_w[1] == "test" && a_w.size() > 2 && a_w[2] == "boss")) {
			// The pick now, every character around with the reason he is or isn't it (never a non-hostile one). test boss: as if
			// every hostile one were a boss (the bosses = off refusal, without going near a real one).
			const bool test = a_w[1] == "test";
			g_testBoss = test;
			const auto                  chrs = Around(a_f, true);
			std::vector<dog::Candidate> cands;
			const auto                  res = RunPick(a_f, chrs, cands, false);
			marking::Camera             cam;
			ReadCamera(cam);
			const Vec3  snake = game::Position(a_f.player);
			std::string out = std::format("dog {}: {} (style {}, the dog at {}){}", test ? "test boss" : "pick",
				res.index ? "PICKED " + NameOf(chrs[*res.index].c) : std::string("no pick: ") + g_lastWhy, Fetching() ? "fetch" : "attack",
				Buddy::HasEstimate() ? "its placement" : "Snake's feet", test ? " [every hostile treated as a boss]" : "");
			// NPC phantoms (PlayerIns: summonable or quest characters like Rogier, invaders) are never even looked at.
			for (const auto& c : game::Characters(65)) {
				if (c.isPlayer && c.chr != a_f.player.chr && !c.dead && Length(c.pos - snake) <= 40.0f) {
					out += std::format("\n  {} (an NPC phantom, PlayerIns) {:.1f} m from Snake: never a candidate", NameOf(c), Length(c.pos - snake));
				}
			}
			for (std::size_t i = 0; i < cands.size() && i < 16; ++i) {
				float      off = 0;
				const auto why = dog::Screen(cands[i], { cam.pos, cam.fwd }, snake, Fetching(), Rules(), &off);
				out += std::format("\n  {} {:.1f} m from Snake, {:.1f} deg off the centre: {}{}{}", NameOf(chrs[i].c), Length(chrs[i].c.pos - snake),
					off * 57.2958f, dog::Name(why), res.index && *res.index == i ? " <- THE PICK" : "", cands[i].hostile ? "" : " (never a candidate)");
			}
			if (test && res.nearest && *res.nearest < chrs.size()) {
				const auto& x = chrs[*res.nearest];
				const auto  eff = dog::EffectOf(proto::kDogKill, x.c.maxHp, 1.0f, true, false, false, Rules(), Combat::CurrentRules(), Cqc::CurrentRules(),
					 Combat::CurrentMelee());
				out += std::format("\n  a bite on {} as a boss: {}", NameOf(x.c), dog::Name(eff.act));
			}
			g_testBoss = false;
			g_lastPick = 0;
			return out;
		}
		if (a_w[1] == "bark") {
			Bark(a_f, "console");
			return Describe();
		}
		if (a_w[1] == "attack" || a_w[1] == "release") {
			// As the guest would send it (tests without MGSV): Connected (or Released) on the pick or a handle.
			proto::GuestEvent e{};
			e.type = proto::kGEvDogAttack;
			proto::DogStyle style = g_style.value_or(proto::kDogBusy);
			std::uint64_t   handle = a_w[1] == "release" && g_engaged ? g_engaged->handle : g_target.load();
			for (std::size_t i = 2; i < a_w.size(); ++i) {
				if (a_w[i].starts_with("0x")) {
					handle = std::strtoull(a_w[i].c_str(), nullptr, 16);
				} else if (a_w[i] == "busy") {
					style = proto::kDogBusy;
				} else if (a_w[i] == "kill") {
					style = proto::kDogKill;
				} else if (a_w[i] == "knockout") {
					style = proto::kDogKnockOut;
				} else if (a_w[i] == "wound") {
					style = proto::kDogWound;
				} else if (a_w[i] == "fetch") {
					style = proto::kDogFetch;
				}
			}
			e.a = proto::DogAttackA(a_w[1] == "release" ? proto::kDogReleased : proto::kDogConnected, style);
			e.pad[0] = static_cast<std::uint32_t>(handle);
			e.pad[1] = static_cast<std::uint32_t>(handle >> 32);
			std::copy_n(a_f.guest.buddyPos, 3, e.v0);
			g_fromConsole = true;
			OnAttack(a_f, e);
			g_fromConsole = false;
			return Describe();
		}
		return "dog [status | on | off | all on|off | pick | test boss | style <s> | attack [busy|kill|knockout|wound|fetch] [0xHANDLE] | release | bark | "
		       "set <key> <value>]";
	}
}
