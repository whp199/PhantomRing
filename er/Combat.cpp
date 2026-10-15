#include "Combat.h"

#include "Addresses.h"
#include "BulletSpawn.h"
#include "CombatLogic.h"
#include "Config.h"
#include "Cqc.h"
#include "DebugDraw.h"
#include "Explosion.h"
#include "Game.h"
#include "Log.h"
#include "Mem.h"
#include "Decoys.h"
#include "Mines.h"
#include "Params.h"
#include "Progression.h"
#include "Puppet.h"
#include "Rounds.h"
#include "Throw.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>

#include <windows.h>

namespace phantom::er::Combat
{
	namespace
	{
		using combat::Capsule;
		using combat::HitEffect;

		// CSChrPhysicsModule hit capsule, loaded from NpcParam hitHeight/hitRadius (fromsoftware-rs
		// physics.rs [V-src]; sanity-checked per character, config fallback). chr_hit_* is the
		// character-vs-character capsule, the second choice.
		constexpr std::uint32_t kPhysHitHeight = 0x2F0;
		constexpr std::uint32_t kPhysHitRadius = 0x2F4;
		constexpr std::uint32_t kPhysChrHitHeight = 0x2E0;
		constexpr std::uint32_t kPhysChrHitRadius = 0x2E4;
		constexpr std::uint32_t kTeamTypes = 79;    // the relation table is [79][79] (0x51B5D0: imul 0x4F)
		constexpr std::int32_t  kStateSleep = 436;  // SpEffectParam stateInfo of ER's sleep build-up rows
		constexpr std::uint32_t kChrSpEffect = 0x178;  // ChrIns -> SpecialEffect (fromsoftware-rs; RemoveSpEffect reads it)
		constexpr std::uint32_t kModResist = 0x20;     // ChrInsModuleContainer -> CSChrResistModule [V live: RTTI]
		// CSChrResistModule [V live, Gatefront soldier 43114010]: +0x10 i32[7] what is left before each status procs (sleep
		// 226 -> 79 -> ... -> 10 while the gas built up), +0x2C i32[7] the thresholds (169 169 226 999 226 226 2015; sleep 226 ->
		// 268 after a sleep: resistCorrectId), +0x64 i32[7] (sleep: 1 after it had procced).
		constexpr std::uint32_t kResistLeft = 0x10;
		constexpr std::uint32_t kResistMax = 0x2C;
		constexpr std::uint32_t kResistGauge = 0x64;
		constexpr std::uint32_t kStatusSleep = 5;      // poison, rot, bleed, death, frost, SLEEP, madness

		using KillFn = void (*)(void* a_chr, bool a_flag);
		using ChangeHpFn = void (*)(void* a_data, std::int32_t a_hp, bool a_flag, float a_f0, float a_f1, bool a_b);
		using ApplySpEffectFn = void (*)(void* a_chr, std::int32_t a_id, bool a_dontSync);
		using RemoveSpEffectFn = void (*)(void* a_chr, std::int32_t a_id);
		using GetTeamTypeFn = std::uint8_t* (*)(void* a_chr, std::uint8_t* a_out);

		struct Target
		{
			std::uint8_t* chr{ nullptr };
			std::uint8_t* data{ nullptr };
			std::uint64_t handle{ 0 };
			Capsule       cap;
			std::int32_t  hp{ 0 };
			std::int32_t  maxHp{ 0 };
			std::int32_t  npc{ 0 };
			std::uint8_t  team{ 0 };
			std::int32_t  sleepResist{ 0 };  // CSChrResistModule (0 = unreadable)
			Quat          orientation;
			bool          hostile{ false };
			bool          capsuleFromParam{ false };
			float         distance{ 0 };

			[[nodiscard]] std::string Name() const { return std::format("npc {} team {} hp {}/{}", npc, team, hp, maxHp); }
		};

		struct Pending
		{
			std::uint8_t* chr{ nullptr };
			std::uint64_t handle{ 0 };
			std::int32_t  hpBefore{ 0 };
			std::int32_t  damage{ 0 };
			std::int32_t  npc{ 0 };
			std::uint16_t correction{ 100 };  // the AtkParam correction the bullet flew with (combat::TopUp)
			std::uint64_t key{ 0 };           // combat::HitKey of the row it flew with (combat::MayFly)
			bool          landedSeen{ false };  // our own HP route hit this target after the bullet had landed:
			std::int32_t  dealtSeen{ 0 };       // what ER had dealt by then (hpBefore no longer shows it)
			ULONGLONG     due{ 0 };
			bool          fallback{ false };  // route auto: give it the HP route if it didn't land
			bool          nonLethal{ false };
			// damage_route = speffect: the damage rides on the bullet as ladder rows (combat::NativeDamage), no top-up.
			bool          ladder{ false };
			std::int32_t  carried{ 0 };       // the ladder rows' sum
			std::int32_t  erExpected{ 0 };    // Elden Ring's own part we planned for
			bool          ladderFallback{ false };  // the ladder is unconfirmed: a missing ladder gets the top-up
			ULONGLONG     firstDropAt{ 0 };   // when Elden Ring's own part of a ladder hit showed (the rows come a few frames later)
			std::string   name;
		};

		params::Table                 g_atk{ params::kAtkParamPc };
		params::Table                 g_bullet{ params::kBullet };
		params::Table                 g_spEffect{ params::kSpEffect };
		params::Patch                 g_patch;
		combat::RouteMonitor          g_monitor;
		combat::TopUp                 g_topUp;
		std::uint64_t                 g_topUps{ 0 }, g_kills{ 0 };
		// damage_route = speffect (docs/COMBAT.md §2 "Damage that syncs"): the ladder, its own route monitor (did the rows'
		// damage arrive?), whether its carrier rows are written (cleared whenever g_patch is restored).
		combat::NativeDamage          g_native;
		combat::RouteMonitor          g_ladderMonitor;
		combat::RouteMonitor::State   g_ladderLastState{ combat::RouteMonitor::State::kUnknown };
		combat::NativeDamage::GuardRule g_lastGuardRule{ combat::NativeDamage::GuardRule::kUnknown };
		std::string                   g_damageRouteOverride;  // console: "" = ini
		bool                          g_ladderPatched{ false };
		std::uint64_t                 g_ladderHits{ 0 }, g_ladderArrived{ 0 }, g_ladderMissing{ 0 }, g_ladderGuarded{ 0 }, g_ladderFallbacks{ 0 }, g_ladderNoSlot{ 0 };
		std::int64_t                  g_ladderWanted{ 0 }, g_ladderDealt{ 0 };  // over landed ladder hits that did not kill
		combat::RateLimiter           g_limiter;
		std::vector<Pending>          g_pending;
		std::string                   g_routeOverride;  // console: "" = ini
		std::uint64_t                 g_hits{ 0 }, g_bulletHits{ 0 }, g_hpHits{ 0 }, g_sleeps{ 0 }, g_fallbacks{ 0 }, g_logged{ 0 };
		std::uint64_t                 g_busyHits{ 0 };
		std::uint64_t                 g_sleepLost{ 0 };  // sleep build-ups that could not ride on a bullet  // HP route because another bullet held the row or the target (combat::MayFly)
		std::uint64_t                 g_shots{ 0 }, g_hitsAtSummary{ 0 };
		double                        g_ratioSum{ 0 };   // dealt / wanted over landed bullet hits (not kills)
		std::uint64_t                 g_ratioCount{ 0 };
		ULONGLONG                     g_lastSummary{ 0 };
		bool                          g_warnedBullet{ false }, g_warnedCapsule{ false };
		combat::RouteMonitor::State   g_lastState{ combat::RouteMonitor::State::kUnknown };
		float                         g_playerMaxHp{ 0 };  // the Tarnished's max HP (combat::Scaling: how big an enemy is)
		std::uint64_t                 g_sharedHits{ 0 }, g_tableHits{ 0 };  // lethal shot hits by MGSV's own share (v8) / the class table
		// Elden Ring refusing our hit bullets (SpawnBullet error 4: its pool is full): each such hit falls to the HP route (no
		// stagger, its sleep lost), so a run of them is warned about (first one, then every 10 s) whatever the log budget.
		std::uint64_t                 g_spawnRefused{ 0 }, g_refusedRun{ 0 };
		ULONGLONG                     g_lastRefusalWarn{ 0 };

		const Addresses& addr() { return Addr(); }

		Vec3 V(const float* a_v) { return { a_v[0], a_v[1], a_v[2] }; }

		template <class... Args>
		void HitLog(std::format_string<Args...> a_fmt, Args&&... a_args)
		{
			if (++g_logged <= 120) {
				Log::Info(a_fmt, std::forward<Args>(a_args)...);
			} else {
				Log::Debug(a_fmt, std::forward<Args>(a_args)...);
			}
		}

		combat::HitRules Rules()
		{
			const auto&      c = Cfg();
			combat::HitRules r;
			r.scaling.unitsPerLife = c.unitsPerLife;
			r.scaling.bigFrom = c.bigFrom;
			r.scaling.bossFrom = c.bossFrom;
			r.scaling.bossLives = c.bossLives;
			r.scaling.playerMaxHp = g_playerMaxHp;
			r.headFraction = c.headFraction;
			r.headshotMult = c.headshotMult;
			r.headshotMultBoss = c.headshotMultBoss;
			r.headshotMinLives = c.headshotMinLives;
			r.shareMult = c.shareMult;
			r.buddyMult = c.buddyMult;
			r.progression = Progression::Factor();
			r.tranqSleep = c.tranqSleep;
			r.tranqSleepHead = c.tranqSleepHead;
			r.tranqShare = c.tranqShare;
			r.tranqShareHead = c.tranqShareHead;
			r.stunPoiseMult = c.stunPoiseMult;
			r.maxDamage = c.maxDamage;
			return r;
		}

		combat::MeleeValues Melee()
		{
			const auto&         c = Cfg();
			combat::MeleeValues m;
			m.punch = c.meleePunch;
			m.kick = c.meleeKick;
			m.throwDamage = c.meleeThrow;
			m.throwPoise = c.meleeThrowPoise;
			m.strikePoise = c.meleeStrikePoise;
			m.chokeSleep = c.chokeSleep;
			m.chokeShare = c.chokeShare;
			return m;
		}

		bool IsChr(const std::uint8_t* a_chr)
		{
			const auto vptr = a_chr ? SafeRead<void*>(a_chr) : std::nullopt;
			const auto& a = addr();
			return vptr && *vptr && (*vptr == a[A::VtEnemyIns] || *vptr == a[A::VtChrIns] || *vptr == a[A::VtPlayerIns]);
		}

		// ---- teams ---------------------------------------------------------------------------------
		std::uint8_t TeamOf(std::uint8_t* a_chr)
		{
			if (addr().Has(A::GetTeamType)) {
				std::uint8_t team = 0;
				addr().Fn<GetTeamTypeFn>(A::GetTeamType)(a_chr, &team);
				return team;
			}
			return SafeRead<std::uint8_t>(a_chr + game::kChrTeam).value_or(0);
		}

		bool Hostile(std::uint8_t a_attacker, std::uint8_t a_target)
		{
			if (a_attacker >= kTeamTypes || a_target >= kTeamTypes) {
				return false;
			}
			const auto& a = addr();
			if (a.HasAll(A::TeamRelationTable, A::VtTeamTypeEnemy, A::VtTeamTypeRival)) {
				auto*      relation = SafePtr(a[A::TeamRelationTable] + (std::size_t(a_attacker) * kTeamTypes + a_target) * 8);
				const auto vptr = relation ? SafeRead<void*>(relation) : std::nullopt;
				return vptr && (*vptr == a[A::VtTeamTypeEnemy] || *vptr == a[A::VtTeamTypeRival]);
			}
			// The table's row for team 1 (the host player) in this exe, read offline: everything but
			// these friends/neutrals is an enemy.
			static constexpr std::uint8_t kNotHostileToPlayer[] = { 0, 1, 2, 3, 4, 5, 10, 13, 14, 15, 23, 26, 28, 30, 46, 47, 78 };
			if (a_attacker == 1) {
				return std::ranges::find(kNotHostileToPlayer, a_target) == std::end(kNotHostileToPlayer);
			}
			return a_attacker != a_target;
		}

		// ---- targets ---------------------------------------------------------------------------------
		// The ladder rows really are sleep build-ups in this regulation (checked once; a mismatch drops sleep, logged).
		bool SleepLadderOk()
		{
			static int s_ok = -1;  // -1 unknown (table not bound yet), 0 no, 1 yes
			if (s_ok >= 0) {
				return s_ok == 1;
			}
			const auto& c = Cfg();
			for (const auto id : { c.sleepSpEffectBase, c.sleepSpEffectBase + c.sleepSpEffectCount - 1 }) {
				auto* row = id > 0 ? g_spEffect.Row(static_cast<std::uint32_t>(id)) : nullptr;
				if (!row) {
					return false;  // try again later
				}
				const auto state = SafeRead<std::uint16_t>(row + params::speffect::kStateInfo).value_or(0);
				const auto power = SafeRead<std::int32_t>(row + params::speffect::kSleepAttackPower).value_or(0);
				if (state != kStateSleep || power <= 0) {
					s_ok = 0;
					Log::Warn("combat: SpEffectParam {} is not a sleep build-up row (stateInfo {}, sleepAttackPower {}); sleep is off -- check [Combat] "
					          "sleep_speffect_*",
						id, state, power);
					return false;
				}
			}
			s_ok = 1;
			return true;
		}

		// ---- damage_route = speffect: the damage ladder (combat::DamageLadder) ---------------------------------------
		combat::DamageLadder Ladder() { return { Cfg().damageSpEffectBase, Cfg().damageSpEffectDigits }; }

		std::string_view ConfiguredDamageRoute()
		{
			return g_damageRouteOverride.empty() ? std::string_view(Cfg().damageRoute) : std::string_view(g_damageRouteOverride);
		}

		// The carrier rows, written once while Snake plays (restored with every other patched row when the puppet stops).
		// Each must be blank in this regulation before we write it -- no damage, no state, no effect, no duration, no target
		// -- or the ladder stays off for the session (another mod or a regulation update took the rows). Fixed values, the
		// same on every machine: another PhantomRing player's game applies the same row ids the same way.
		bool LadderReady()
		{
			static int s_ok = -1;  // -1 unknown (table not bound yet), 0 no, 1 yes
			if (g_ladderPatched) {
				return true;
			}
			if (s_ok == 0) {
				return false;
			}
			namespace sp = params::speffect;
			const auto  l = Ladder();
			const int   rows = combat::DamageLadderRows(l);
			if (l.base <= 0) {
				return false;
			}
			std::vector<std::uint8_t*> ptrs;
			for (int k = 0; k < rows; ++k) {
				const auto id = static_cast<std::uint32_t>(l.base + k);
				auto*      row = g_spEffect.Row(id);
				if (!row) {
					return false;  // the table is not bound yet: try again later
				}
				const bool blank = SafeRead<std::int32_t>(row + sp::kChangeHpPoint).value_or(-1) == 0 &&
				                   SafeRead<float>(row + sp::kChangeHpRate).value_or(-1.0f) == 0.0f &&
				                   SafeRead<float>(row + sp::kEffectEndurance).value_or(-1.0f) == 0.0f &&
				                   SafeRead<float>(row + sp::kMotionInterval).value_or(-1.0f) == 0.0f &&
				                   SafeRead<std::uint16_t>(row + sp::kStateInfo).value_or(1) == 0 &&
				                   SafeRead<std::int32_t>(row + sp::kSleepAttackPower).value_or(1) == 0 &&
				                   SafeRead<std::int32_t>(row + sp::kVfxId).value_or(0) == -1 &&
				                   SafeRead<std::int32_t>(row + sp::kReplaceSpEffectId).value_or(0) == -1 &&
				                   SafeRead<std::int32_t>(row + sp::kCycleSpEffectId).value_or(0) == -1 &&
				                   SafeRead<std::uint8_t>(row + sp::kTargetBits).value_or(1) == 0 &&
				                   (SafeRead<std::uint8_t>(row + sp::kBits259).value_or(0x10) & 0x12) == 0;
				if (!blank) {
					s_ok = 0;
					Log::Warn("combat: SpEffectParam {} is not a blank row in this regulation (the damage ladder's carrier) -- damage_route speffect "
					          "is off; check [Combat] damage_speffect_base",
						id);
					return false;
				}
				ptrs.push_back(row);
			}
			// [Combat] speffect_template: a row of Elden Ring's copied whole into each carrier first (its target bits, priority,
			// category: everything but the damage).
			const int     tplId = Cfg().speffectTemplate;
			std::uint8_t* tpl = tplId > 0 ? g_spEffect.Row(static_cast<std::uint32_t>(tplId)) : nullptr;
			if (tplId > 0 && !tpl) {
				Log::Warn("combat: [Combat] speffect_template {} is no SpEffectParam row -- the carriers stay blank rows", tplId);
			}
			for (int k = 0; k < rows; ++k) {
				const std::int32_t value = combat::DamageRungValue(l.base + k, l);
				if (tpl) {
					std::vector<std::uint8_t> copy(params::kSpEffect.rowSize);
					if (SafeReadBytes(tpl, copy.data(), copy.size())) {
						g_patch.Write(ptrs[k], 0, copy.data(), copy.size());
					}
				} else {
					g_patch.Set<std::uint8_t>(ptrs[k], sp::kTargetBits, sp::kLadderTargetBits);
					g_patch.Set<std::uint8_t>(ptrs[k], sp::kTargetBits2, sp::kLadderTargetBits2);
				}
				g_patch.Set<std::int32_t>(ptrs[k], sp::kChangeHpPoint, value);
			}
			if (tpl) {
				Log::Info("combat: damage ladder carriers are copies of SpEffectParam {} (but for their damage)", tplId);
			}
			if (s_ok < 0) {
				Log::Info("combat: damage ladder ready -- SpEffectParam {}..{} carry 1..{} HP (damage_route speffect, UNVERIFIED live)", l.base,
					l.base + rows - 1, combat::DamageLadderMax(l));
			}
			s_ok = 1;
			g_ladderPatched = true;
			return true;
		}

		void RestorePatches()
		{
			g_patch.Restore();
			g_ladderPatched = false;
		}

		combat::NativeDamage::Rules NativeRules()
		{
			const auto& c = Cfg();
			return { c.speffectBulletShare, static_cast<std::uint16_t>(c.speffectFirstCorrection), static_cast<double>(c.speffectGuardMs) };
		}

		// The ladder carries this hit's damage: speffect configured, its rows ready, not given up for the session.
		bool LadderRoute()
		{
			return ConfiguredDamageRoute() == "speffect" && g_ladderMonitor.Current() != combat::RouteMonitor::State::kFailed && LadderReady();
		}

		// Sleep resistance as Elden Ring has it for this character right now (area scaling, growth after a sleep).
		std::int32_t SleepResist(std::uint8_t* a_chr)
		{
			auto*      modules = SafePtr(a_chr + game::kChrModules);
			auto*      resist = modules ? SafePtr(modules + kModResist) : nullptr;
			const auto v = resist ? SafeRead<std::int32_t>(resist + kResistMax + 4 * kStatusSleep) : std::nullopt;  // the threshold, not what's left
			return v && *v > 0 && *v <= 9999 ? *v : 0;
		}

		bool Sane(float a_height, float a_radius)
		{
			return std::isfinite(a_height) && std::isfinite(a_radius) && a_radius >= 0.08f && a_radius <= 10.0f && a_height >= 0.2f && a_height <= 40.0f;
		}

		std::vector<Target> Targets(const Frame& a_f, const Vec3& a_near, float a_radius)
		{
			std::vector<Target> out;
			if (!a_f.player) {
				return out;
			}
			const std::uint8_t me = TeamOf(a_f.player.chr);
			const bool         all = Cfg().targets == "all";
			for (const auto& c : game::Characters(128)) {
				if (c.chr == a_f.player.chr || c.dead || c.hp <= 0 || !Finite(c.pos)) {
					continue;
				}
				const float d = Length(c.pos - a_near);
				if (d > a_radius + 12.0f) {
					continue;
				}
				auto* modules = SafePtr(c.chr + game::kChrModules);
				auto* phys = modules ? SafePtr(modules + game::kModPhysics) : nullptr;
				Target t;
				t.chr = c.chr;
				t.data = modules ? SafePtr(modules + game::kModData) : nullptr;
				t.handle = c.handle;
				t.hp = c.hp;
				t.maxHp = c.maxHp;
				t.npc = c.npcParam;
				t.distance = d;
				t.orientation = c.orientation;
				float h = phys ? SafeRead<float>(phys + kPhysHitHeight).value_or(NAN) : NAN;
				float r = phys ? SafeRead<float>(phys + kPhysHitRadius).value_or(NAN) : NAN;
				t.capsuleFromParam = Sane(h, r);
				if (!t.capsuleFromParam) {
					h = phys ? SafeRead<float>(phys + kPhysChrHitHeight).value_or(NAN) : NAN;
					r = phys ? SafeRead<float>(phys + kPhysChrHitRadius).value_or(NAN) : NAN;
					if (!Sane(h, r)) {
						h = Cfg().capsuleHeight;
						r = Cfg().capsuleRadius;
						if (!g_warnedCapsule) {
							g_warnedCapsule = true;
							Log::Warn("combat: npc {}'s physics module has no sane hit capsule (+0x2F0/+0x2F4); using {:.2f} x {:.2f} m", c.npcParam, h, r);
						}
					}
				}
				t.cap = combat::ChrCapsule(c.pos, h, r);
				t.team = TeamOf(c.chr);
				t.sleepResist = SleepResist(c.chr);
				t.hostile = all || Hostile(me, t.team);
				out.push_back(t);
			}
			return out;
		}

		// ---- applying hits -------------------------------------------------------------------------
		std::string_view ConfiguredRoute() { return g_routeOverride.empty() ? std::string_view(Cfg().route) : std::string_view(g_routeOverride); }

		bool BulletRouteUsable()
		{
			const auto& a = addr();
			return a.HasAll(A::SpawnBullet, A::CSBulletManager) && a.Instance(A::CSBulletManager) && params::Available();
		}

		std::string_view EffectiveRoute()
		{
			const auto route = ConfiguredRoute();
			if (route == "hp") {
				return "hp";
			}
			if (route == "auto" && g_monitor.Current() == combat::RouteMonitor::State::kFailed) {
				return "hp";
			}
			return BulletRouteUsable() ? std::string_view("bullet") : std::string_view("hp");
		}

		void ApplyHp(std::uint8_t* a_chr, std::uint8_t* a_data, std::int32_t a_damage, bool a_nonLethal = false)
		{
			const auto hp = a_data ? SafeRead<std::int32_t>(a_data + game::kDataHp) : std::nullopt;
			if (!hp || *hp <= 0 || a_damage <= 0) {
				return;
			}
			if (a_nonLethal) {
				a_damage = std::min(a_damage, *hp - 1);  // stun/sleep: never the killing blow
				if (a_damage <= 0) {
					return;
				}
			}
			const std::int32_t left = *hp - a_damage;
			const auto&        a = addr();
			if (left <= 0 && a.Has(A::ChrInsKill)) {
				// ChrIns::Kill marks the character dead and runs its death even when Elden Ring keeps it alive (its
				// ChangeHp(0) holds an event-immortal boss or NPC at 1 HP: the NoDead bits, data+0x19A bit 7, SpEffect
				// states 0x8F/0xEB -- VERIFIED in Ghidra, 0x140438810 -> 0x140437CA0). So ask ChangeHp first and only
				// kill what it let reach 0.
				if (a.Has(A::ChangeHp)) {
					a.Fn<ChangeHpFn>(A::ChangeHp)(a_data, 0, false, 0.0f, 1.0f, true);
					if (SafeRead<std::int32_t>(a_data + game::kDataHp).value_or(1) > 0) {
						HitLog("combat: {} is immortal right now (Elden Ring kept it at 1 HP) -- not killed", static_cast<void*>(a_chr));
						return;
					}
				} else {
					const auto debug = SafeRead<std::uint8_t>(a_data + game::kDataDebug).value_or(0);
					const auto flags = SafeRead<std::uint8_t>(a_data + 0x19A).value_or(0);
					if ((debug & 0x01) || (flags & 0x80)) {
						SafeWrite<std::int32_t>(a_data + game::kDataHp, 1);
						return;
					}
				}
				a.Fn<KillFn>(A::ChrInsKill)(a_chr, false);
			} else if (a.Has(A::ChangeHp)) {
				// The Kill call's own arguments (0x3EE74F: r8b 0, xmm3 0, [rsp+20] 1.0f, [rsp+28] 1).
				a.Fn<ChangeHpFn>(A::ChangeHp)(a_data, std::max(1, left), false, 0.0f, 1.0f, true);
			} else {
				SafeWrite<std::int32_t>(a_data + game::kDataHp, std::max(1, left));
			}
		}

		// The atkPhys / atkFire a hit's bullet is patched with.
		std::uint16_t BulletAmount(const HitEffect& a_e)
		{
			return static_cast<std::uint16_t>(std::clamp<long>(std::lround(static_cast<float>(a_e.damage) * Cfg().bulletDamageMult), 0, 65535));
		}

		// a_slots: the bullet's spEffectId0..4 -- the sleep rungs, then the damage ladder's rows (damage_route speffect).
		// a_amount: the atkPhys / atkFire written (ER ignores it: combat::TopUp; it keeps the row's numbers in its HitKey).
		bool SpawnHitBullet(const Frame& a_f, const Target& a_t, const HitEffect& a_e, const Vec3& a_at, const Vec3& a_dir, const Vec3& a_from, std::uint16_t a_correction,
			const std::vector<std::int32_t>& a_slots, std::uint16_t a_amount)
		{
			const auto& a = addr();
			const auto& c = Cfg();
			auto*       manager = a.Instance(A::CSBulletManager);
			auto*       bulletRow = g_bullet.Row(c.bulletId);
			const auto  atkId = bulletRow ? SafeRead<std::int32_t>(bulletRow + params::bullet::kAtkId).value_or(-1) : -1;
			auto*       atkRow = atkId > 0 ? g_atk.Row(static_cast<std::uint32_t>(atkId)) : nullptr;
			if (!manager || !bulletRow || !atkRow) {
				if (!g_warnedBullet) {
					g_warnedBullet = true;
					Log::Warn("combat: bullet route unavailable (manager {}, Bullet {} {}, AtkParam_Pc {} {}) -- {} / {}", static_cast<void*>(manager), c.bulletId,
						static_cast<void*>(bulletRow), atkId, static_cast<void*>(atkRow), g_bullet.Describe(), g_atk.Describe());
				}
				return false;
			}
			// This hit's numbers into the bullet's attack row (restored when the puppet stops).
			if (c.bulletHideSfx) {
				g_patch.Set<std::int32_t>(bulletRow, params::bullet::kSfxIdBullet, -1);
			}
			const auto amount = a_amount;
			// Physical, or fire for D-Walker's flame (Elden Ring's fire absorption applies; the other stays 0).
			// The corrections are what ER's damage follows (its weapon x correction: combat::TopUp); atkPhys/atkFire only
			// choose the element.
			g_patch.Set<std::uint16_t>(atkRow, params::atk::kAtkPhys, a_e.fire ? std::uint16_t(0) : amount);
			g_patch.Set<std::uint16_t>(atkRow, params::atk::kAtkPhysCorrection, a_correction);
			g_patch.Set<std::uint16_t>(atkRow, params::atk::kAtkFire, a_e.fire ? amount : std::uint16_t(0));
			g_patch.Set<std::uint16_t>(atkRow, params::atk::kAtkFireCorrection, a_correction);
			g_patch.Set<std::uint16_t>(atkRow, params::atk::kAtkStam, static_cast<std::uint16_t>(std::clamp(a_e.poise, 0.0f, 999.0f)));
			g_patch.Set<float>(atkRow, params::atk::kAtkSuperArmor, std::max(0.0f, a_e.poise));
			g_patch.Set<float>(atkRow, params::atk::kAtkSuperArmorCorrection, 100.0f);
			g_patch.Set<std::uint8_t>(atkRow, params::atk::kDmgLevel, a_e.dmgLevel);
			// Sleep rides on the bullet, as on Elden Ring's own sleep bullets (Bullet 2270260 -> SpEffect 25006, the Sleep Pot's
			// mist -> 500640): its spEffectId0..4 are applied by the hit (combat::SleepRungs). 0 = none, the row's own value.
			for (std::uint32_t slot = 0; slot < 5; ++slot) {
				g_patch.Set<std::int32_t>(bulletRow, params::bullet::kSpEffectId0 + 4 * slot, slot < a_slots.size() ? a_slots[slot] : 0);
			}

			// Spawn just in front of the hit point, aimed through it (never behind the muzzle).
			const Vec3  dir = Normalize(a_dir);
			const float back = std::min(Cfg().bulletOffset, std::max(0.0f, Length(a_at - a_from) - 0.05f));
			const Vec3  pos = a_at - dir * back;
			alignas(16) std::uint8_t data[spawn::kSize];
			spawn::Fill(data, SafeRead<std::uint64_t>(a_f.player.chr + game::kChrHandle).value_or(~0ull), static_cast<std::int32_t>(c.bulletId), a_t.handle, pos, dir);
			std::int32_t out = -1, error = 0;
			a.Fn<spawn::SpawnBulletFn>(A::SpawnBullet)(manager, &out, data, &error);
			if (out == -1) {
				++g_spawnRefused;
				++g_refusedRun;
				const auto now = ::GetTickCount64();
				if (g_refusedRun == 1 || now - g_lastRefusalWarn >= 10000) {
					g_lastRefusalWarn = now;
					Log::Warn("combat: Elden Ring refused our hit bullet (SpawnBullet({}) error {}: 3 = bad bullet id, 4 = construction failed) -- {} in a "
					          "row, {} in all; those hits take the HP route (no stagger, no sleep)",
						c.bulletId, error, g_refusedRun, g_spawnRefused);
				}
				return false;
			}
			if (g_refusedRun) {
				Log::Info("combat: hit bullets spawn again (after {} refused)", g_refusedRun);
				g_refusedRun = 0;
			}
			return true;
		}

		// One landed hit on a_t: damage, poise and sleep build-up through the route.
		void Apply(const Frame& a_f, const Target& a_t, HitEffect a_e, const Vec3& a_at, const Vec3& a_dir, const Vec3& a_from, std::string_view a_what)
		{
			const auto& c = Cfg();
			const auto  sleep = a_e.sleep > 0 || a_e.sleepShare > 0 ? combat::SleepPower(a_e.sleep, a_e.sleepShare, a_t.sleepResist) : 0;
			const auto  rungs = sleep > 0 && SleepLadderOk() ? combat::SleepRungs(sleep, c.sleepSpEffectBase, c.sleepSpEffectStep, c.sleepSpEffectCount)
			                                               : std::vector<std::int32_t>{};
			if (a_e.damage <= 0 && a_e.poise <= 0 && rungs.empty()) {
				return;
			}
			if (!g_limiter.Allow(static_cast<double>(::GetTickCount64()))) {
				Log::Debug("combat: {} on {} dropped (rate limit, {} so far)", a_what, a_t.Name(), g_limiter.Dropped());
				return;
			}
			if (a_e.nonLethal && a_e.damage >= a_t.hp) {
				a_e.damage = std::max(0, a_t.hp - 1);  // stun/sleep knock out, they don't kill
			}
			++g_hits;
			std::string how;
			const auto  route = EffectiveRoute();
			// A sleep-only hit (a dart, the gas) deals nothing: correction 0, no flinch. A blast that gives everyone the same
			// numbers brings its own (combat::StunBlastEffect).
			const auto    sleepCorrection = static_cast<std::uint16_t>(std::clamp(c.sleepCorrection, 0, 100));
			std::uint16_t correction = a_e.correction ? std::min<std::uint16_t>(a_e.correction, 100)
			                           : a_e.damage <= 0 ? sleepCorrection
			                                             : (c.topUp ? g_topUp.Correction(a_t.npc, a_e.damage) : std::uint16_t(100));
			if (a_e.nonLethal) {
				// Elden Ring's own part of a stun/sleep hit (its weapon x the correction) is never clamped: it must not be able to
				// kill either (combat::NonLethalCorrection).
				correction = combat::NonLethalCorrection(correction, a_e.damage, a_t.hp, g_topUp.Reference(a_t.npc), sleepCorrection);
			}
			// The row carries the rungs, not the exact amount: hits that round to the same rungs may fly together (a gas
			// cloud over several enemies of one kind).
			std::int32_t rungSleep = 0;
			for (const auto id : rungs) {
				rungSleep += (id - c.sleepSpEffectBase + 1) * c.sleepSpEffectStep;
			}
			// damage_route = speffect: the damage rides on the bullet as ladder rows in the slots the sleep rungs leave free, and
			// Elden Ring's own part shrinks to a share of it (combat::NativeDamage). Stun and sleep hits keep the top-up: their
			// part must never kill, and ER's own part of them is not under our control to the HP.
			combat::NativePlan        plan;
			std::vector<std::int32_t> ladderRows;
			std::int32_t              carried = 0;
			bool                      ladder = false;
			if (route == "bullet" && a_e.damage > 0 && !a_e.nonLethal && LadderRoute()) {
				plan = g_native.Plan(a_t.npc, a_t.chr, a_e.damage, static_cast<double>(::GetTickCount64()), NativeRules());
				const std::size_t free = rungs.size() < 5 ? 5 - rungs.size() : 0;
				ladderRows = combat::DamageRungs(plan.ladder, Ladder(), free);
				carried = combat::DamageOfRungs(ladderRows, Ladder());
				if (plan.ladder > 0 && ladderRows.empty()) {
					++g_ladderNoSlot;  // no slot left: this hit is topped up as before
				} else {
					ladder = true;
					correction = plan.correction;
				}
			}
			std::vector<std::int32_t> slots = rungs;
			if (ladder) {
				slots.insert(slots.end(), ladderRows.begin(), ladderRows.end());
			}
			const std::uint16_t amount = ladder ? static_cast<std::uint16_t>(std::min(carried, 65535)) : BulletAmount(a_e);
			const std::uint64_t key =
				ladder && carried > 65535 ? combat::kUniqueKey : combat::HitKey(amount, correction, a_e.fire, a_e.poise, a_e.dmgLevel, rungSleep);
			// One patched row for every bullet: only bullets with the same numbers fly together, and one with damage per
			// target (combat::MayFly); the others take the HP route (and lose their sleep: it needs the attack).
			std::vector<combat::InFlight> flying;
			for (const auto& p : g_pending) {
				flying.push_back({ p.chr, p.key, p.damage > 0 });
			}
			const bool busy = route == "bullet" && !combat::MayFly(flying, a_t.chr, key, a_e.damage > 0);
			if (busy) {
				++g_busyHits;
			}
			if (route == "bullet" && !busy && SpawnHitBullet(a_f, a_t, a_e, a_at, a_dir, a_from, correction, slots, amount)) {
				++g_bulletHits;
				if (!rungs.empty()) {
					++g_sleeps;
				}
				g_ladderHits += ladder ? 1 : 0;
				Pending p;
				p.chr = a_t.chr;
				p.handle = a_t.handle;
				p.hpBefore = a_t.hp;
				p.damage = a_e.damage;
				p.npc = a_t.npc;
				p.correction = correction;
				p.key = key;
				p.nonLethal = a_e.nonLethal;
				// Nothing to measure on a hit without damage: it only holds the row while it flies (~50 ms live).
				p.due = ::GetTickCount64() + static_cast<ULONGLONG>(a_e.damage > 0 ? std::max(50, c.verifyMs) : 120);
				p.fallback = ConfiguredRoute() == "auto" && g_monitor.Current() == combat::RouteMonitor::State::kUnknown;
				p.name = a_t.Name();
				p.ladder = ladder;
				p.carried = carried;
				p.erExpected = plan.erExpected;
				p.ladderFallback = ladder && g_ladderMonitor.Current() == combat::RouteMonitor::State::kUnknown;
				g_pending.push_back(std::move(p));  // poise/sleep only too: its row must not change under it
				how = std::format("bullet {} poise {:.0f} lv {}{}{}", a_e.damage, a_e.poise, a_e.dmgLevel,
					ladder ? std::format(" = ER ~{} ({}%) + ladder {} ({} row{}{})", plan.erExpected, correction, carried, ladderRows.size(),
								 ladderRows.size() == 1 ? "" : "s", plan.guardFactor < 1.0f ? std::format(", shield up: x{:.2f}", plan.guardFactor) : std::string())
						   : std::string(),
					rungs.empty() ? std::string() : std::format(" sleep +{} (resist {}, {} rung{})", sleep, a_t.sleepResist, rungs.size(), rungs.size() > 1 ? "s" : ""));
			} else {
				if (!rungs.empty()) {
					++g_sleepLost;
					how = std::format("sleep +{} LOST ({})", sleep, route != "bullet" ? "it needs the bullet route" : busy ? "another bullet holds the row" : "bullet failed");
				}
				if (a_e.damage > 0) {
					// A bullet of ours on this target that already landed keeps what ER dealt; our HP must not count as it.
					const auto before = a_t.data ? SafeRead<std::int32_t>(a_t.data + game::kDataHp).value_or(0) : 0;
					for (auto& other : g_pending) {
						if (other.chr == a_t.chr && other.damage > 0 && !other.landedSeen && before < other.hpBefore) {
							other.landedSeen = true;
							other.dealtSeen = other.hpBefore - std::max(0, before);
						}
					}
					ApplyHp(a_t.chr, a_t.data, a_e.damage, a_e.nonLethal);
					++g_hpHits;
					const auto after = a_t.data ? SafeRead<std::int32_t>(a_t.data + game::kDataHp).value_or(0) : 0;
					for (auto& other : g_pending) {
						if (other.chr == a_t.chr) {
							other.hpBefore = std::min(other.hpBefore, after);  // not a bullet landing
						}
					}
					how += std::format("{}hp -{}{}", how.empty() ? "" : ", ", a_e.damage,
						busy ? " (a bullet is in flight)" : route == "bullet" ? " (bullet failed)" : "");
				}
			}
			HitLog("combat: {} -> {}{} at ({:.2f}, {:.2f}, {:.2f}): {}", a_what, a_t.Name(), a_e.headshot ? " HEADSHOT" : "", a_at.x, a_at.y, a_at.z,
				how.empty() ? "nothing applied" : how);
		}

		Vec3 SurfacePoint(const Capsule& a_c, const Vec3& a_toward)
		{
			const Vec3 axis = combat::ClosestOnSegment(a_c.a, a_c.b, a_toward);
			const Vec3 out = a_toward - axis;
			const float l = Length(out);
			return l > 1e-4f ? axis + out * (std::min(l, a_c.r) / l) : axis;
		}

		void Blast(const Frame& a_f, const Vec3& a_centre, float a_radius, std::string_view a_what, std::uint32_t a_kind = proto::kAmmoLethal)
		{
			if (a_kind == proto::kAmmoSmoke) {
				return;  // smoke / flare (guest v4): nothing to hurt
			}
			const auto rules = Rules();
			std::vector<std::pair<Target, float>> caught;
			for (const auto& t : Targets(a_f, a_centre, a_radius)) {
				const float d = combat::CapsuleDistance(t.cap, a_centre);
				if (t.hostile && d <= a_radius) {
					caught.emplace_back(t, std::max(0.0f, d));
				}
			}
			// Stun and sleep gas: one set of numbers for everyone inside, so their bullets fly together (combat::
			// StunBlastEffect / GasBlastEffect); a sleep-gas puff comes every ~0.5 s while MGSV's cloud lasts.
			std::optional<HitEffect> same;
			if (a_kind == proto::kAmmoTranq) {
				std::vector<std::int32_t> resists;
				for (const auto& [t, d] : caught) {
					resists.push_back(t.sleepResist);
				}
				const auto& c = Cfg();
				same = combat::GasBlastEffect(c.gasSleep, c.gasShare, resists, c.sleepSpEffectStep * c.sleepSpEffectCount * 5);
			} else if (a_kind == proto::kAmmoStun) {
				same = combat::StunBlastEffect(rules);
			}
			for (const auto& [t, d] : caught) {
				// The blast reaches it from above (its bullet comes down on the top of its capsule), not along the line from
				// the centre: a raised shield faces the shooter and blocked the frontal bullet of a grenade at its feet
				// (live: 5 of a normal 42 HP).
				const Vec3 top = t.cap.b + Vec3{ 0, t.cap.r, 0 };
				const Vec3 side = top - a_centre;
				const Vec3 dir = Normalize(Vec3{ side.x * 0.25f, -1.0f, side.z * 0.25f });
				Apply(a_f, t, same ? *same : combat::ExplosionEffect(d, a_radius, t.maxHp, rules), top, dir, top - dir * 1.0f, a_what);
			}
			const auto n = caught.size();
			Log::Debug("combat: {} r {:.1f} m at ({:.2f}, {:.2f}, {:.2f}) caught {}", a_what, a_radius, a_centre.x, a_centre.y, a_centre.z, n);
		}

		// Active SpEffect ids of a character ([ChrIns+0x178] SpecialEffect: +8 head; entry +8 id, +0x30 next; fromsoftware-rs).
		std::string ActiveSpEffects(std::uint8_t* a_chr)
		{
			std::string out;
			auto*       module = SafePtr(a_chr + kChrSpEffect);
			auto*       e = module ? SafePtr(module + 8) : nullptr;
			for (int i = 0; e && i < 64; ++i) {
				out += std::format("{} ", SafeRead<std::int32_t>(e + 8).value_or(-1));
				e = SafePtr(e + 0x30);
			}
			return out.empty() ? "none" : out;
		}

		// CSChrResistModule ([modules+0x20]): resistances and build-up gauges by status (poison, rot, bleed, death, frost,
		// sleep, madness).
		std::string ResistLine(std::uint8_t* a_chr)
		{
			auto* modules = SafePtr(a_chr + game::kChrModules);
			auto* resist = modules ? SafePtr(modules + kModResist) : nullptr;
			if (!resist) {
				return "no resist module";
			}
			std::string out = "left";
			for (std::uint32_t i = 0; i < 7; ++i) {
				out += std::format(" {}", SafeRead<std::int32_t>(resist + kResistLeft + 4 * i).value_or(-1));
			}
			out += " | resist";
			for (std::uint32_t i = 0; i < 7; ++i) {
				out += std::format(" {}", SafeRead<std::int32_t>(resist + kResistMax + 4 * i).value_or(-1));
			}
			out += " | +0x64";
			for (std::uint32_t i = 0; i < 7; ++i) {
				out += std::format(" {}", SafeRead<std::int32_t>(resist + kResistGauge + 4 * i).value_or(-1));
			}
			out += " | raw +0x48..";
			for (std::uint32_t off = 0x48; off < 0xA0; off += 4) {
				out += std::format(" {:08x}", SafeRead<std::uint32_t>(resist + off).value_or(0));
			}
			return out;
		}

		std::optional<Target> Nearest(const Frame& a_f, std::size_t a_index, float a_radius)
		{
			auto list = Targets(a_f, game::Position(a_f.player), a_radius);
			std::ranges::sort(list, {}, &Target::distance);
			if (a_index >= list.size()) {
				return std::nullopt;
			}
			return list[a_index];
		}
	}

	// ---- events ------------------------------------------------------------------------------------
	void OnShot(const Frame& a_f, const proto::GuestEvent& a_e, bool a_buddy)
	{
		const char* who = a_buddy ? "buddy" : "Snake";
		if (!Puppet::Active() || !a_f.player) {
			return;
		}
		g_playerMaxHp = static_cast<float>(game::MaxHp(a_f.player));
		const auto& anchor = Puppet::CurrentAnchor();
		const Vec3  muzzle = anchor.PointToEr(V(a_e.v0));
		const Vec3  dir = Normalize(anchor.DirToEr(V(a_e.v1)));
		if (!Finite(muzzle) || Length(dir) < 0.5f) {
			return;
		}
		++g_shots;
		const std::uint32_t bulletId = a_e.pad[0] & 0x7FFFFFFF;  // guest extension (protocol comment at kGEvFire)
		// D-Walker's flamethrower: buddy fire without a bullet, carrying its reach in pad[1] (mm, guest extension):
		// a stream as long as that, a body anywhere near it burns (the stream has width), fire damage.
		const bool  flame = a_buddy && bulletId == 0 && a_e.pad[1] > 0;
		const float range = flame ? std::clamp(static_cast<float>(a_e.pad[1]) / 1000.0f, 2.0f, 30.0f) : Cfg().traceRange;
		const float widen = flame ? 0.6f : 0.0f;
		// (v8) MGSV's own damage of this round as a share of a standard soldier's life (Snake's rounds only: a buddy's pad[1] is
		// the flame's reach).
		const float share = !a_buddy ? static_cast<float>(a_e.pad[1]) / 10000.0f : 0.0f;
		if (flame) {
			// Elden Ring's own flames along the stream (MGSV's fire is only drawn in MGSV): seen from outside and riding.
			Explosion::Flame(a_f, muzzle + dir * 0.6f, dir);
		}
		Vec3        worldHit;
		const bool  hitWorld = game::CastRay(muzzle, dir * range, worldHit, a_f.player.chr, Cfg().rayFilter);
		const float worldDist = hitWorld ? Length(worldHit - muzzle) : range;

		// The first character capsule along the ray before the wall (ER's ray may itself stop on the body
		// it hit, so a little past the wall still counts).
		std::optional<Target> target;
		float                 best = worldDist + 0.35f;
		for (const auto& t : Targets(a_f, muzzle, range)) {
			auto cap = t.cap;
			cap.r += widen;
			if (const auto hit = combat::RayCapsule(muzzle, dir, best, cap); hit && *hit < best) {
				best = *hit;
				target = t;
			}
		}
		const Vec3 end = target ? muzzle + dir * best : (hitWorld ? worldHit : muzzle + dir * range);
		DebugDraw::AddTracer(muzzle, end, target || hitWorld, a_buddy);

		const std::uint32_t weapon = a_e.a & 0xFFFF;
		const std::uint32_t ammo = a_e.a >> 16;
		const bool          explosive = (a_e.pad[0] & 0x80000000u) != 0;
		auto                cls = flame ? combat::RoundClass::kFlame : combat::ClassOfBullet(bulletId);
		if (explosive) {
			cls = combat::RoundClass::kExplosive;
		}
		const std::string what = std::format("{} {} (weapon {} bullet {} x{}{})", who, combat::Name(cls), weapon, bulletId, a_e.b,
			share > 0.0f ? std::format(", MGSV share {:.3f}", share) : "");
		if (g_shots <= 50) {
			Log::Info("{} -> {} at ({:.2f}, {:.2f}, {:.2f})", what, target ? target->Name() : (hitWorld ? "world" : "nothing"), end.x, end.y, end.z);
		} else {
			Log::Debug("{} -> {} at ({:.2f}, {:.2f}, {:.2f})", what, target ? target->Name() : (hitWorld ? "world" : "nothing"), end.x, end.y, end.z);
		}
		if (!flame) {
			Explosion::Muzzle(a_f, muzzle, dir, ammo == proto::kAmmoTranq || cls == combat::RoundClass::kTranq);  // [Combat] muzzle_flash (roadmap 5.4)
		}
		if (cls == combat::RoundClass::kExplosive) {
			// A rocket / grenade round flies from the muzzle and goes off where it meets the world or an enemy (er/Rounds.h);
			// without a flight for it, at the impact point at once as before.
			if (Rounds::Launch(a_f, bulletId, muzzle, dir, Cfg().explosionRadius, what)) {
				return;
			}
			if (target || hitWorld) {
				// Elden Ring never sees MGSV's own blast: draw one of ER's at the impact (on a surface already).
				Explosion::Show(a_f, explosion::Kind::kLethal, end, Cfg().explosionRadius, false, what);
				Explosion::Shrapnel(a_f, end - dir * 0.3f, Cfg().explosionRadius, what);
				if (Cfg().damage) {
					Blast(a_f, end, Cfg().explosionRadius, what);
				}
			}
			return;
		}
		if (!target && hitWorld && !flame && cls != combat::RoundClass::kTranq && cls != combat::RoundClass::kNone && ammo != proto::kAmmoTranq) {
			// A round into the world: a crate, a pot, a cart there breaks as Elden Ring's own projectiles break it.
			Explosion::Break(a_f, worldHit, dir, what);
		}
		if (!Cfg().damage) {
			return;
		}
		if (!target) {
			return;
		}
		if (!target->hostile) {
			Log::Debug("combat: {} hit {} (not hostile to the Tarnished): no damage", what, target->Name());
			return;
		}
		const auto effect = combat::ShotEffect(cls, ammo, a_e.b, combat::HeightFraction(target->cap, end), target->maxHp, a_buddy, Rules(), share);
		if (effect.damage > 0 && !flame) {
			++(effect.shared ? g_sharedHits : g_tableHits);
		}
		Apply(a_f, *target, effect, end, dir, muzzle, what);
	}

	void OnMelee(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (!Puppet::Active() || !a_f.player) {
			return;
		}
		const auto& anchor = Puppet::CurrentAnchor();
		const Vec3  centre = anchor.PointToEr(V(a_e.v0));
		const float radius = std::clamp(static_cast<float>(a_e.b) / 1000.0f, 0.2f, 3.0f);
		Vec3        facing = anchor.DirToEr(V(a_e.v1));
		facing.y = 0;
		const Vec3 snake = anchor.pe + Vec3{ 0, 1.0f, 0 };
		Log::Info("Snake CQC kind {} (MGSV attack {}) r {:.2f} m at ER ({:.2f}, {:.2f}, {:.2f})", a_e.a, a_e.pad[0], radius, centre.x, centre.y, centre.z);
		if (!Cfg().damage || !Finite(centre)) {
			return;
		}
		// The nearest character the strike sphere touches, or failing that the nearest in reach in front.
		const float           arc = Cfg().meleeArcDeg * std::numbers::pi_v<float> / 180.0f;
		std::optional<Target> best;
		float                 bestD = 1e9f;
		for (const auto& t : Targets(a_f, centre, Cfg().meleeRange + radius)) {
			if (!t.hostile) {
				continue;
			}
			const float d = combat::CapsuleDistance(t.cap, centre);
			const bool  touched = d <= radius || combat::InArc(snake, facing, Cfg().meleeRange, arc, t.cap);
			if (touched && d < bestD) {
				bestD = d;
				best = t;
			}
		}
		if (!best) {
			// A strike that met no one: a crate or a pot there breaks (the strike sphere, MGSV's own reach).
			Explosion::Break(a_f, centre, Length(facing) > 1e-3f ? Normalize(facing) : Vec3{ 0, -1, 0 }, std::format("CQC kind {}", a_e.a));
			return;
		}
		const Vec3 at = SurfacePoint(best->cap, centre);
		Vec3       dir = at - snake;
		dir = Length(dir) > 1e-3f ? Normalize(dir) : Normalize(facing);
		Apply(a_f, *best, combat::MeleeEffect(a_e.a, best->maxHp, Melee(), Rules()), at, dir, snake, std::format("CQC kind {}", a_e.a));
	}

	bool HitCharacter(const Frame& a_f, std::uint64_t a_handle, const Vec3& a_from, float a_reach, const combat::HitEffect& a_e, std::string_view a_what)
	{
		if (!Puppet::Active() || !a_f.player || !Cfg().damage || !Finite(a_from)) {
			return false;
		}
		for (const auto& t : Targets(a_f, a_from, a_reach)) {
			if (t.handle != a_handle || combat::CapsuleDistance(t.cap, a_from) > a_reach) {
				continue;
			}
			const Vec3 at = SurfacePoint(t.cap, a_from);
			Vec3       dir = at - a_from;
			dir = Length(dir) > 1e-3f ? Normalize(dir) : Vec3{ 0, 0, 1 };
			Apply(a_f, t, a_e, at, dir, a_from, a_what);
			return true;
		}
		return false;
	}

	bool RowBusy() { return EffectiveRoute() == "bullet" && !g_pending.empty(); }

	combat::HitRules    CurrentRules() { return Rules(); }
	combat::MeleeValues CurrentMelee() { return Melee(); }

	void OnExplosion(const Frame& a_f, const proto::GuestEvent& a_e)
	{
		if (!Puppet::Active() || !a_f.player) {
			Log::Info("guest explosion kind {} radius {} mm (no puppet)", a_e.a, a_e.b);
			return;
		}
		const Vec3  centre = Puppet::CurrentAnchor().PointToEr(V(a_e.v0));
		const float radius = a_e.b ? std::clamp(static_cast<float>(a_e.b) / 1000.0f, 0.5f, 30.0f) : Cfg().explosionRadius;
		Log::Info("guest explosion kind {} radius {:.1f} m at ER ({:.2f}, {:.2f}, {:.2f})", a_e.a, radius, centre.x, centre.y, centre.z);
		if (const auto kind = explosion::FromAmmo(a_e.a); kind && Finite(centre)) {
			// MGSV's blast centre comes through the anchor from its flat sandbox: it is dropped onto ER's ground.
			Explosion::Show(a_f, *kind, centre, radius, true, std::format("explosion kind {}", a_e.a));
		}
		if (a_e.a == proto::kAmmoLethal && Finite(centre)) {
			Explosion::Shrapnel(a_f, centre, radius, std::format("explosion kind {}", a_e.a));
		}
		if (Cfg().damage && Finite(centre)) {
			Blast(a_f, centre, radius, std::format("explosion kind {}", a_e.a), a_e.a);
		}
	}

	void BlastAt(const Frame& a_f, std::uint32_t a_kind, const Vec3& a_centre, float a_radius, bool a_onGround, std::string_view a_what)
	{
		if (!Puppet::Active() || !a_f.player || !Finite(a_centre)) {
			return;
		}
		if (const auto kind = explosion::FromAmmo(a_kind)) {
			Explosion::Show(a_f, *kind, a_centre, a_radius, a_onGround, a_what);
		}
		if (a_kind == proto::kAmmoLethal) {
			Explosion::Shrapnel(a_f, a_centre, a_radius, a_what);  // the blast breaks the crates and carts around it
		}
		if (Cfg().damage) {
			Blast(a_f, a_centre, a_radius, a_what, a_kind);
		}
	}

	bool HostileToPlayer(const Frame& a_f, std::uint8_t a_team) { return a_f.player && Hostile(TeamOf(a_f.player.chr), a_team); }

	bool Asleep(std::uint8_t* a_chr)
	{
		constexpr std::uint16_t kStateHelpless = 434;  // SpEffectParam stateInfo of row 54, on a sleeping (or stance-broken) character
		auto*                   module = a_chr ? SafePtr(a_chr + kChrSpEffect) : nullptr;
		auto*                   e = module ? SafePtr(module + 8) : nullptr;
		bool                    helpless = false, sleepRow = false;
		for (int i = 0; e && i < 64 && !(helpless && sleepRow); ++i) {
			const auto id = SafeRead<std::int32_t>(e + 8).value_or(-1);
			if (auto* row = id > 0 ? g_spEffect.Row(static_cast<std::uint32_t>(id)) : nullptr) {
				const auto state = SafeRead<std::uint16_t>(row + params::speffect::kStateInfo).value_or(0);
				helpless = helpless || state == kStateHelpless;
				sleepRow = sleepRow || state == kStateSleep;
			}
			e = SafePtr(e + 0x30);
		}
		return helpless && sleepRow;
	}

	std::vector<Foe> HostilesNear(const Frame& a_f, const Vec3& a_centre, float a_radius)
	{
		std::vector<Foe> out;
		for (const auto& t : Targets(a_f, a_centre, a_radius)) {
			if (t.hostile && combat::CapsuleDistance(t.cap, a_centre) <= a_radius) {
				out.push_back({ t.cap, t.Name(), t.chr, t.handle, t.orientation, t.maxHp });
			}
		}
		return out;
	}

	void Tick(const Frame& a_f)
	{
		const auto now = ::GetTickCount64();
		if (a_f.player) {
			g_playerMaxHp = static_cast<float>(game::MaxHp(a_f.player));
		}
		g_limiter.Configure(Cfg().maxHitsPerSecond, std::max(5.0f, Cfg().maxHitsPerSecond / 2.0f));
		g_monitor.SetFailLimit(Cfg().verifyFailures);
		g_ladderMonitor.SetFailLimit(Cfg().verifyFailures);
		// Did the bullets land? (The target lost HP or died; a vanished target is not counted.) Looked at every frame: the
		// rest of the damage goes on the frame after Elden Ring's own hit (it used to wait verify_ms, 300 ms, for every
		// hit: a killing shot dropped the enemy late); verify_ms only bounds how long a miss is waited for.
		for (auto it = g_pending.begin(); it != g_pending.end();) {
			const bool valid = IsChr(it->chr) && SafeRead<std::uint64_t>(it->chr + game::kChrHandle).value_or(0) == it->handle;
			auto*      modules = valid ? SafePtr(it->chr + game::kChrModules) : nullptr;
			auto*      data = modules ? SafePtr(modules + game::kModData) : nullptr;
			const auto hp = data ? SafeRead<std::int32_t>(data + game::kDataHp) : std::nullopt;
			const bool dead = valid && (SafeRead<std::uint8_t>(it->chr + game::kChrFlags1C5).value_or(0) & 0x80) != 0;
			const bool landed = dead || it->landedSeen || (hp && *hp < it->hpBefore);
			if (valid && !landed && now < it->due) {
				++it;
				continue;
			}
			// A ladder hit: Elden Ring applies the bullet's SpEffect rows a few frames after its own hit (live 2026-10-14: a
			// soldier 221 -> 220 at 131 ms, the rows' 46 by 199 ms; judged on the first drop they read MISSING and were topped up
			// again). Their damage is waited for, [Combat] speffect_wait_ms at most.
			if (valid && landed && !dead && !it->landedSeen && it->ladder && it->carried > 0 && hp && it->hpBefore - std::max(0, *hp) < it->carried) {
				if (!it->firstDropAt) {
					it->firstDropAt = now;
				}
				if (now - it->firstDropAt < static_cast<ULONGLONG>(std::max(0, Cfg().speffectWaitMs))) {
					++it;
					continue;
				}
			}
			if (valid && it->damage <= 0) {
				it = g_pending.erase(it);  // poise only: it only held the row (no HP to measure, not a route test)
				continue;
			}
			if (valid) {
				const bool fallback = g_monitor.Report(landed) && it->fallback;
				if (landed) {
					// What ER made of the hit (its own weapon-based damage, guard, absorption) -- diagnostics -- and the
					// rest of the wanted damage on top (combat::TopUp; [Combat] top_up).
					const std::int32_t dealt = it->landedSeen ? it->dealtSeen : it->hpBefore - std::max(0, hp.value_or(0));
					if (!dead && it->damage > 0) {
						g_ratioSum += static_cast<double>(dealt) / it->damage;
						++g_ratioCount;
					}
					std::int32_t extra = 0;
					std::string  ladderNote;
					if (it->ladder && !dead) {
						// The whole hit was Elden Ring's: its own part + the ladder rows. Did the rows' damage arrive? (A guarded hit
						// whose rows did not arrive is Elden Ring blocking them, not a failure: combat::NativeDamage.)
						const auto o = g_native.Learn(it->npc, it->chr, it->correction, it->carried, dealt, static_cast<double>(now),
							g_ladderMonitor.Current() == combat::RouteMonitor::State::kConfirmed);
						// Only arrived rows confirm the ladder; a guarded miss is no evidence and gets no top-up; nothing carried (a
						// shield's share of 0, ER's part already all of it) tests nothing (combat::ReportLadder).
						const bool fill = combat::ReportLadder(g_ladderMonitor, o, it->carried) && it->ladderFallback;
						g_ladderArrived += o.ladderLanded ? 1 : 0;
						g_ladderMissing += it->carried > 0 && !o.ladderLanded ? 1 : 0;
						g_ladderGuarded += o.guarded ? 1 : 0;
						if (fill && hp && *hp > 0) {
							// Unconfirmed and missing: this hit gets the rest the old way (as the bullet route's own fallback).
							extra = std::max(0, it->damage - dealt);
							if (extra > 0) {
								ApplyHp(it->chr, data, extra, false);
								++g_ladderFallbacks;
								const auto after = SafeRead<std::int32_t>(data + game::kDataHp).value_or(0);
								for (auto& other : g_pending) {
									if (&other != &*it && other.chr == it->chr) {
										other.hpBefore = std::min(other.hpBefore, after);
									}
								}
							}
						}
						g_ladderWanted += it->damage;
						g_ladderDealt += dealt;
						ladderNote = std::format(" = ER {} (expected {}, correction {}%) + ladder {} {}{}{}", o.erPart, it->erExpected, it->correction, it->carried,
							it->carried <= 0 ? "(none)" : o.ladderLanded ? "ARRIVED" : "MISSING", o.guarded ? std::format(", GUARDED ({:.0f}% through)", 100.0f * o.guardShare) : "",
							extra > 0 ? std::format(", +{} topped up (ladder unconfirmed)", extra) : "");
					} else if (!dead && hp && *hp > 0 && Cfg().topUp && !it->ladder) {
						extra = g_topUp.Extra(it->npc, it->damage, dealt, it->correction);
						if (extra > 0) {
							ApplyHp(it->chr, data, extra, it->nonLethal);
							++g_topUps;
							const bool killed = (SafeRead<std::uint8_t>(it->chr + game::kChrFlags1C5).value_or(0) & 0x80) != 0 ||
							                    SafeRead<std::int32_t>(data + game::kDataHp).value_or(1) <= 0 || extra >= *hp;
							g_kills += killed ? 1 : 0;
							// Later bullets on this target must not count our HP as theirs.
							const auto after = SafeRead<std::int32_t>(data + game::kDataHp).value_or(0);
							for (auto& other : g_pending) {
								if (&other != &*it && other.chr == it->chr) {
									other.hpBefore = std::min(other.hpBefore, after);
								}
							}
						}
					} else if (dead) {
						++g_kills;
					}
					HitLog("combat: bullet on {} landed after {} ms: hp {} -> {} (wanted -{}{}){}", it->name,
						now + static_cast<ULONGLONG>(std::max(50, Cfg().verifyMs)) - it->due, it->hpBefore, hp.value_or(0), it->damage, dead ? ", dead" : "",
						it->ladder ? ladderNote
						: extra > 0 ? std::format(", +{} topped up ({}% of a normal hit got through)", extra,
										  g_topUp.Reference(it->npc) ? 100 * dealt / g_topUp.Reference(it->npc) : 100)
									: std::string());
				} else if (fallback && hp && *hp > 0) {
					ApplyHp(it->chr, data, it->damage, it->nonLethal);
					++g_fallbacks;
					// Our own fallback damage must not make a later bullet on this target look like it landed.
					const auto after = SafeRead<std::int32_t>(data + game::kDataHp).value_or(0);
					for (auto& other : g_pending) {
						if (&other != &*it && other.chr == it->chr) {
							other.hpBefore = std::min(other.hpBefore, after);
						}
					}
					HitLog("combat: bullet on {} did not land within {} ms; hp fallback -{}", it->name, Cfg().verifyMs, it->damage);
				}
			}
			it = g_pending.erase(it);
		}
		if (g_monitor.Current() != g_lastState) {
			g_lastState = g_monitor.Current();
			if (g_lastState == combat::RouteMonitor::State::kConfirmed) {
				Log::Info("combat: bullet route CONFIRMED (Elden Ring applied our bullet hit)");
			} else if (g_lastState == combat::RouteMonitor::State::kFailed) {
				Log::Warn("combat: {} bullet hits in a row did not land -- switching to the hp route for this session (`combat route bullet` to retry)",
					Cfg().verifyFailures);
			}
		}
		if (g_ladderMonitor.Current() != g_ladderLastState) {
			g_ladderLastState = g_ladderMonitor.Current();
			if (g_ladderLastState == combat::RouteMonitor::State::kConfirmed) {
				Log::Info("combat: damage ladder CONFIRMED (Elden Ring applied the bullet's fixed-damage SpEffect rows)");
			} else if (g_ladderLastState == combat::RouteMonitor::State::kFailed) {
				Log::Warn("combat: the damage ladder's rows did not arrive on {} hits in a row -- damage_route topup for this session (`combat "
				          "damage_route speffect` to retry)",
					Cfg().verifyFailures);
			}
		}
		if (g_native.Guard() != g_lastGuardRule) {
			g_lastGuardRule = g_native.Guard();
			Log::Info("combat: damage ladder on a guarded hit: {}", combat::Name(g_lastGuardRule));
		}
		if (g_hits != g_hitsAtSummary && now - g_lastSummary > 10000) {
			g_lastSummary = now;
			g_hitsAtSummary = g_hits;
			Log::Info("combat: {}", Describe());
		}
		if (!Puppet::Active() && !g_patch.Empty()) {
			RestorePatches();  // belt and braces: OnPuppetStop normally does this
		}
		Explosion::Tick(a_f);
	}

	void OnPuppetStop()
	{
		Cqc::OnPuppetStop();  // held enemies go before anything else is put back
		Throw::OnPuppetStop();
		Rounds::OnPuppetStop();
		Mines::OnPuppetStop();
		Decoys::OnPuppetStop();
		Explosion::OnPuppetStop();
		if (!g_patch.Empty()) {
			RestorePatches();
			Log::Info("combat: bullet/AtkParam rows restored");
		}
		g_pending.clear();
	}

	std::string DescribeDamageRoute();

	std::string Describe()
	{
		const std::string ratio = g_ratioCount ? std::format(", ER dealt {:.0f}% of the wanted damage", 100.0 * g_ratioSum / static_cast<double>(g_ratioCount)) : "";
		return std::format("route {} ({}; bullet {} {}: {} landed / {} missed{}), {} shots, {} hits: {} bullet ({} topped up), {} hp ({} while a bullet flew, "
		                   "{} refused by Elden Ring{}), {} hp fallbacks, {} sleep ({} lost), {} kills; lethal shot hits {} by MGSV's share / {} by class; {} "
		                   "rate-dropped; {}; {}; {}\n{}",
			EffectiveRoute(), ConfiguredRoute(), Cfg().bulletId, combat::Name(g_monitor.Current()), g_monitor.Landed(), g_monitor.Missed(), ratio, g_shots, g_hits,
			g_bulletHits, g_topUps, g_hpHits, g_busyHits, g_spawnRefused, g_refusedRun ? std::format(", {} in a row NOW", g_refusedRun) : "", g_fallbacks, g_sleeps,
			g_sleepLost, g_kills, g_sharedHits, g_tableHits, g_limiter.Dropped(), g_bullet.Describe(), g_atk.Describe(), g_spEffect.Describe(), Rounds::Describe()) +
		       "\n" + DescribeDamageRoute();
	}

	std::string DescribeDamageRoute()
	{
		const auto l = Ladder();
		return std::format("damage route {} ({}; ladder SpEffectParam {}..{} {}, {}): {} ladder hits, rows arrived {} / missing {}, guarded {}, {} topped up while "
		                   "unconfirmed, {} without a free slot{}; on a guarded hit: {}",
			ConfiguredDamageRoute() == "speffect" && g_ladderMonitor.Current() != combat::RouteMonitor::State::kFailed ? "speffect" : "topup",
			g_damageRouteOverride.empty() ? "ini" : "console", l.base, l.base + combat::DamageLadderRows(l) - 1, g_ladderPatched ? "written" : "not written",
			combat::Name(g_ladderMonitor.Current()), g_ladderHits, g_ladderArrived, g_ladderMissing, g_ladderGuarded, g_ladderFallbacks, g_ladderNoSlot,
			g_ladderWanted > 0 ? std::format(", dealt {:.0f}% of the wanted damage", 100.0 * static_cast<double>(g_ladderDealt) / static_cast<double>(g_ladderWanted))
							   : std::string(),
			combat::Name(g_native.Guard()));
	}

	std::string Console(const std::vector<std::string>& a_w, const Frame& a_f)
	{
		const std::string sub = a_w.size() > 1 ? a_w[1] : "status";
		auto              arg = [&](std::size_t a_i, double a_default) { return a_w.size() > a_i ? std::strtod(a_w[a_i].c_str(), nullptr) : a_default; };
		if (sub == "status") {
			return Describe();
		}
		if (sub == "set" && a_w.size() > 3) {
			// Live tuning (not saved): combat set tranq_share|tranq_share_head|gas_share|choke_share|sleep_correction|boss_lives|boss_from|big_from|share_mult <v>
			auto&       c = Cfg();
			const float v = std::strtof(a_w[3].c_str(), nullptr);
			if (a_w[2] == "tranq_share") {
				c.tranqShare = v;
			} else if (a_w[2] == "tranq_share_head") {
				c.tranqShareHead = v;
			} else if (a_w[2] == "gas_share") {
				c.gasShare = v;
			} else if (a_w[2] == "choke_share") {
				c.chokeShare = v;
			} else if (a_w[2] == "sleep_correction") {
				c.sleepCorrection = static_cast<int>(v);
			} else if (a_w[2] == "boss_lives") {
				c.bossLives = v;
			} else if (a_w[2] == "boss_from") {
				c.bossFrom = v;
			} else if (a_w[2] == "big_from") {
				c.bigFrom = v;
			} else if (a_w[2] == "share_mult") {
				c.shareMult = v;
			} else if (a_w[2] == "speffect_template") {
				c.speffectTemplate = static_cast<int>(v);
				RestorePatches();  // the carriers are written again (from the template) by the next ladder hit
			} else {
				return "combat set tranq_share|tranq_share_head|gas_share|choke_share|sleep_correction|boss_lives|boss_from|big_from|share_mult|"
				       "speffect_template <value>";
			}
			return std::format("{} = {}", a_w[2], v);
		}
		if (sub == "route" && a_w.size() > 2) {
			const auto& r = a_w[2];
			if (r != "auto" && r != "bullet" && r != "hp" && r != "ini") {
				return "route auto|bullet|hp|ini";
			}
			g_routeOverride = r == "ini" ? "" : r;
			g_monitor.Reset();
			return Describe();
		}
		if (sub == "damage_route") {
			// `combat damage_route [topup|speffect|ini]`: how the bullet route delivers the wanted damage (docs/COMBAT.md §2).
			if (a_w.size() > 2) {
				const auto& r = a_w[2];
				if (r != "topup" && r != "speffect" && r != "ini") {
					return "damage_route topup|speffect|ini";
				}
				g_damageRouteOverride = r == "ini" ? "" : r;
				g_ladderMonitor.Reset();
			}
			return DescribeDamageRoute();
		}
		if (!a_f.player) {
			return "no player";
		}
		if ((sub == "shoot" || sub == "strike") && a_w.size() > 4 && Puppet::Active()) {
			// Test shots without MGSV's aim: `combat shoot <rifle|handgun|sniper|buck|stun|tranq|rocket|grenade|flame> <x> <y> <z> [share]`
			// fires one round of that kind from Snake's chest at an Elden Ring point (as kGEvFire / D-Walker's kGEvBuddyFire would);
			// `combat strike <x> <y> <z>` is a CQC punch there (kGEvMelee).
			const auto& anchor = Puppet::CurrentAnchor();
			const int   first = sub == "shoot" ? 3 : 2;
			const Vec3  to{ static_cast<float>(arg(first, 0)), static_cast<float>(arg(first + 1, 0)), static_cast<float>(arg(first + 2, 0)) };
			const Vec3  chest = anchor.pe + Vec3{ 0, 1.4f, 0 };
			const Vec3  dir = Normalize(to - chest);
			proto::GuestEvent e{};
			auto put = [](float* a_out, const Vec3& a_v) {
				a_out[0] = a_v.x;
				a_out[1] = a_v.y;
				a_out[2] = a_v.z;
			};
			if (sub == "strike") {
				e.type = proto::kGEvMelee;
				e.a = proto::kMeleePunch;
				e.b = 300;
				put(e.v0, anchor.PointToMgsv(to));
				put(e.v1, anchor.DirToMgsv(dir));
				e.pad[0] = 80;
				OnMelee(a_f, e);
				return std::format("CQC punch at ({:.2f}, {:.2f}, {:.2f})", to.x, to.y, to.z);
			}
			struct K
			{
				const char*   name;
				std::uint32_t bullet;
				std::uint32_t ammo;
				std::uint32_t pellets;
				bool          explosive;
			};
			static constexpr K kKinds[] = { { "rifle", 15, proto::kAmmoLethal, 1, false }, { "handgun", 5, proto::kAmmoLethal, 1, false },
				{ "sniper", 37, proto::kAmmoLethal, 1, false }, { "buck", 18, proto::kAmmoLethal, 8, false }, { "stun", 22, proto::kAmmoStun, 1, false },
				{ "tranq", 10, proto::kAmmoTranq, 1, false }, { "rocket", 59, proto::kAmmoLethal, 1, true }, { "grenade", 23, proto::kAmmoLethal, 1, true },
				{ "flame", 0, proto::kAmmoLethal, 1, false } };
			const auto* k = std::ranges::find_if(kKinds, [&](const K& a_k) { return a_w[2] == a_k.name; });
			if (k == std::end(kKinds)) {
				return "combat shoot <rifle|handgun|sniper|buck|stun|tranq|rocket|grenade|flame> <x> <y> <z> [share]";
			}
			const bool flame = k->bullet == 0;
			e.type = flame ? proto::kGEvBuddyFire : proto::kGEvFire;
			e.a = (flame ? 4u : 0x123u) | (k->ammo << 16);
			e.b = k->pellets;
			put(e.v0, anchor.PointToMgsv(chest));
			put(e.v1, anchor.DirToMgsv(dir));
			e.pad[0] = k->bullet | (k->explosive ? 0x80000000u : 0u);
			e.pad[1] = flame ? 12000u : static_cast<std::uint32_t>(std::lround(std::clamp(arg(first + 3, 0.0), 0.0, 20.0) * 10000.0));
			OnShot(a_f, e, flame);
			return std::format("{} fired at ({:.2f}, {:.2f}, {:.2f}), {:.1f} m{}", k->name, to.x, to.y, to.z, Length(to - chest),
				e.pad[1] && !flame ? std::format(", MGSV share {:.3f}", e.pad[1] / 10000.0) : "");
		}
		if (sub == "aimat" && a_w.size() > 4 && Puppet::Active()) {
			// Test steering: the MGSV yaw (absolute, for fake_mgsv's `!move`; relative to Snake's facing and the pitch, for its
			// `!fire`) from Snake's chest toward an Elden Ring point, and how far it is.
			const auto& anchor = Puppet::CurrentAnchor();
			const Vec3  to{ static_cast<float>(arg(2, 0)), static_cast<float>(arg(3, 0)), static_cast<float>(arg(4, 0)) };
			const Vec3  from = anchor.pe + Vec3{ 0, 1.4f, 0 };
			const Vec3  dm = anchor.DirToMgsv(to - from);
			const float yaw = std::atan2(dm.x, dm.z);
			const float deg = 180.0f / std::numbers::pi_v<float>;
			// fake_mgsv's `!cam <phase>` looks along -(cos phase, sin phase): this phase puts the camera behind Snake looking at it.
			return std::format("MGSV yaw {:.1f} deg (relative to Snake {:.1f}), pitch {:.1f} deg, {:.1f} m; fake camera phase {:.2f}", yaw * deg,
				WrapPi(yaw - a_f.guest.rootYaw) * deg, std::atan2(dm.y, std::hypot(dm.x, dm.z)) * deg, Length(to - from), std::atan2(-dm.z, -dm.x));
		}
		if (sub == "self" && a_w.size() > 2) {
			// Diagnostics: a SpEffect on the Tarnished himself (`combat self <id> [off]`), and his active ones.
			const auto  id = static_cast<std::int32_t>(arg(2, 0));
			const auto& a = addr();
			if (!a.HasAll(A::ApplySpEffect, A::RemoveSpEffect) || id <= 0) {
				return "ApplySpEffect/RemoveSpEffect unavailable or bad id";
			}
			a.Fn<RemoveSpEffectFn>(A::RemoveSpEffect)(a_f.player.chr, id);
			if (a_w.size() < 4 || a_w[3] != "off") {
				a.Fn<ApplySpEffectFn>(A::ApplySpEffect)(a_f.player.chr, id, true);
			}
			return std::format("the Tarnished's SpEffects: {}", ActiveSpEffects(a_f.player.chr));
		}
		if (sub == "targets") {
			auto list = Targets(a_f, game::Position(a_f.player), 40.0f);
			std::ranges::sort(list, {}, &Target::distance);
			std::string out = std::format("{} characters within 40 m (Tarnished team {}):", list.size(), TeamOf(a_f.player.chr));
			for (std::size_t i = 0; i < list.size() && i < 16; ++i) {
				const auto& t = list[i];
				out += std::format("\n {:2} {} {:.1f} m {}{} capsule r {:.2f} h {:.2f} ({})", i, t.Name(), t.distance, t.hostile ? "HOSTILE" : "friendly",
					Asleep(t.chr) ? " ASLEEP" : "",
					t.cap.r, t.cap.b.y - t.cap.a.y + 2 * t.cap.r, t.capsuleFromParam ? "hit" : "fallback");
				if (Puppet::Active()) {
					// Where to aim from Snake's chest, as MGSV sees it: fake_mgsv's `!fire <kind> 1 <yaw> <pitch>`.
					const auto& anchor = Puppet::CurrentAnchor();
					const Vec3  dm = anchor.DirToMgsv((t.cap.a + t.cap.b) * 0.5f - (anchor.pe + Vec3{ 0, 1.4f, 0 }));
					const float yaw = WrapPi(std::atan2(dm.x, dm.z) - a_f.guest.rootYaw) * 180.0f / std::numbers::pi_v<float>;
					const float pitch = std::atan2(dm.y, std::hypot(dm.x, dm.z)) * 180.0f / std::numbers::pi_v<float>;
					out += std::format(" aim {:.0f} {:.0f}", yaw, pitch);
				}
			}
			return out;
		}
		const auto index = static_cast<std::size_t>(arg(2, 0));
		const auto t = Nearest(a_f, index, 40.0f);
		if (!t) {
			return std::format("no character #{} within 40 m (see `combat targets`)", index);
		}
		// `... above`: the bullet comes down on the top of the target (as a blast's does) instead of from the Tarnished,
		// so a raised shield can't take it.
		const bool above = a_w.back() == "above";
		const Vec3 me = above ? t->cap.b + Vec3{ 0, t->cap.r + 1.0f, 0 } : game::Position(a_f.player) + Vec3{ 0, 1.3f, 0 };
		const Vec3 at = above ? t->cap.b + Vec3{ 0, t->cap.r, 0 } : SurfacePoint(t->cap, me);
		const Vec3 dir = Normalize(at - me);
		if (sub == "hit") {
			HitEffect e;
			e.damage = static_cast<std::int32_t>(arg(3, 50));
			e.poise = static_cast<float>(arg(4, 20));
			e.dmgLevel = e.poise >= 60 ? 3 : (e.poise >= 30 ? 2 : 1);
			Apply(a_f, *t, e, at, dir, me, "console hit");
			return std::format("hit {} with {} damage / {:.0f} poise via {}; `combat` shows whether it landed after {} ms", t->Name(), e.damage, e.poise,
				EffectiveRoute(), Cfg().verifyMs);
		}
		if (sub == "sleep") {
			HitEffect e;
			e.sleep = static_cast<std::int32_t>(arg(3, 100));
			Apply(a_f, *t, e, at, dir, me, "console sleep");
			return std::format("sleep +{} on {}", e.sleep, t->Name());
		}
		if (sub == "speffect" && a_w.size() > 3) {
			// Diagnostics: apply any SpEffect row to target n directly (ChrIns::ApplySpEffect). Not how sleep is given: a
			// build-up row applied this way builds nothing (VERIFIED live); it has to ride on an attack (SpawnHitBullet).
			const auto id = static_cast<std::int32_t>(arg(3, 0));
			const auto& a = addr();
			if (!a.HasAll(A::ApplySpEffect, A::RemoveSpEffect) || id <= 0) {
				return "ApplySpEffect/RemoveSpEffect unavailable or bad id";
			}
			a.Fn<RemoveSpEffectFn>(A::RemoveSpEffect)(t->chr, id);
			a.Fn<ApplySpEffectFn>(A::ApplySpEffect)(t->chr, id, true);
			return std::format("SpEffect {} applied to {}; now active: {}", id, t->Name(), ActiveSpEffects(t->chr));
		}
		if (sub == "effects") {
			return std::format("{}: {}", t->Name(), ActiveSpEffects(t->chr));
		}
		if (sub == "resist") {
			return std::format("{}: {}", t->Name(), ResistLine(t->chr));
		}
		if (sub == "team") {
			return std::format("{}: team {} (+0x6C {}), Tarnished team {}, hostile {}", t->Name(), t->team, SafeRead<std::uint8_t>(t->chr + game::kChrTeam).value_or(0),
				TeamOf(a_f.player.chr), Hostile(TeamOf(a_f.player.chr), t->team));
		}
		return "combat [status] | route auto|bullet|hp|ini | targets | hit <n> [damage [poise]] [above] | sleep <n> [power] [above] | team <n> | resist <n> | "
		       "effects <n> | speffect <n> <id> | shoot <rifle|handgun|sniper|buck|stun|tranq|rocket|grenade|flame> x y z [share] | strike x y z | "
		       "aimat x y z | self <id> [off]";
	}
}
