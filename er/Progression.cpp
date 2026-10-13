#include "Progression.h"

#include "CombatLogic.h"
#include "Config.h"
#include "Game.h"
#include "Log.h"
#include "Mem.h"
#include "Params.h"
#include "Puppet.h"

#include <cmath>
#include <format>

#include <windows.h>

namespace phantom::er::Progression
{
	namespace
	{
		constexpr std::uint32_t kPlayerGameData = 0x580;   // PlayerIns: PlayerGameData*
		constexpr std::uint32_t kPgdMaxHp = 0x14;          // u32 current_max_hp
		constexpr std::uint32_t kPgdAttributes = 0x3C;     // u32 x 8 vigor .. arcane
		constexpr std::uint32_t kPgdLevel = 0x68;          // u32
		constexpr std::uint32_t kPgdChrAsm = 0x2B0 + 0x6C;  // EquipGameData +0x6C ChrAsm
		constexpr std::uint32_t kAsmRightSlot = 0x10;      // u32 0..2: right_weapon_slot
		constexpr std::uint32_t kAsmParamIds = 0x7C;       // i32[22]

		params::Table g_weapons{ params::kEquipParamWeapon };
		params::Table g_reinforce{ params::kReinforceParamWeapon };
		const void*   g_pgdVt{ nullptr };
		float         g_factor{ 1.0f };
		std::uint64_t g_lastRead{ 0 };
		std::string   g_lastWhy;
		std::string   g_lastWeaponWhy;
		int           g_lastLevel{ -1 };
		int           g_lastWeapon{ -2 };
		combat::ProgressionStats g_stats;

		combat::ProgressionRules Rules()
		{
			const auto&              c = Cfg();
			combat::ProgressionRules r;
			r.mode = combat::ProgressionModeOf(c.progression);
			r.levelReference = c.levelReference;
			r.levelExponent = c.levelExponent;
			r.weaponBonus = c.weaponBonus;
			r.minFactor = c.progressionMin;
			r.maxFactor = c.progressionMax;
			return r;
		}

		std::uint8_t* GameData(const game::Player& a_p, std::string& a_why)
		{
			auto*      pgd = SafePtr(a_p.chr + kPlayerGameData);
			const auto vptr = pgd ? SafeRead<const void*>(pgd) : std::nullopt;
			if (!vptr) {
				a_why = "no PlayerGameData at PlayerIns+0x580";
				return nullptr;
			}
			if (!g_pgdVt && game::RttiNameOfVtable(*vptr) == "CS::PlayerGameData") {
				g_pgdVt = *vptr;
			}
			if (*vptr != g_pgdVt) {
				a_why = std::format("PlayerIns+0x580 is a {}, not CS::PlayerGameData", game::RttiNameOfVtable(*vptr));
				return nullptr;
			}
			return pgd;
		}
	}

	Readout Read(const Frame& a_f)
	{
		Readout r;
		if (!a_f.player || !a_f.inGame) {
			r.why = "no player in the world";
			return r;
		}
		auto* pgd = GameData(a_f.player, r.why);
		if (!pgd) {
			return r;
		}
		r.maxHp = static_cast<int>(SafeRead<std::uint32_t>(pgd + kPgdMaxHp).value_or(0));
		r.level = static_cast<int>(SafeRead<std::uint32_t>(pgd + kPgdLevel).value_or(0));
		for (int i = 0; i < 8; ++i) {
			r.attributes[i] = static_cast<int>(SafeRead<std::uint32_t>(pgd + kPgdAttributes + 4 * i).value_or(0));
		}
		// Two cross-checks: the max HP is the data module's, and the level is the attributes' sum - 79.
		if (r.maxHp != game::MaxHp(a_f.player)) {
			r.why = std::format("PlayerGameData+0x14 max HP {} is not the data module's {}", r.maxHp, game::MaxHp(a_f.player));
			return r;
		}
		if (!combat::LevelMatchesAttributes(r.level, r.attributes)) {
			r.why = std::format("level {} is not the attributes' sum - 79 ({} {} {} {} {} {} {} {})", r.level, r.attributes[0], r.attributes[1], r.attributes[2],
				r.attributes[3], r.attributes[4], r.attributes[5], r.attributes[6], r.attributes[7]);
			return r;
		}
		r.valid = true;
		// The right-hand armament in use, its upgrade and the highest one of its kind (ReinforceParamWeapon).
		auto* chrAsm = pgd + kPgdChrAsm;
		r.rightSlot = static_cast<int>(SafeRead<std::uint32_t>(chrAsm + kAsmRightSlot).value_or(99));
		if (r.rightSlot < 0 || r.rightSlot > 2) {
			r.weaponWhy = std::format("right-hand slot {} is not 0..2", r.rightSlot);
			return r;
		}
		r.weaponId = SafeRead<std::int32_t>(chrAsm + kAsmParamIds + 4 * (1 + 2 * r.rightSlot)).value_or(-1);
		auto* row = r.weaponId >= 0 ? g_weapons.Row(static_cast<std::uint32_t>(combat::WeaponRowOf(r.weaponId))) : nullptr;
		if (!row) {
			r.weaponWhy = std::format("weapon {} has no EquipParamWeapon row {} ({})", r.weaponId, combat::WeaponRowOf(r.weaponId), g_weapons.Describe());
			return r;
		}
		r.reinforceType = SafeRead<std::int16_t>(row + params::weapon::kReinforceTypeId).value_or(-1);
		r.upgrade = combat::UpgradeOf(r.weaponId);
		r.maxUpgrade = r.reinforceType >= 0 ? combat::MaxUpgrade(r.reinforceType, [](int a_id) { return g_reinforce.Row(static_cast<std::uint32_t>(a_id)) != nullptr; }) : 0;
		if (r.maxUpgrade <= 0 || r.upgrade > r.maxUpgrade) {
			r.weaponWhy = std::format("weapon {}: upgrade +{} of reinforce type {} with {} levels ({})", r.weaponId, r.upgrade, r.reinforceType, r.maxUpgrade,
				g_reinforce.Describe());
			r.upgrade = -1;
			r.maxUpgrade = 0;
		}
		return r;
	}

	void Tick(const Frame& a_f)
	{
		const auto rules = Rules();
		if (rules.mode == combat::ProgressionMode::kOff) {
			g_factor = 1.0f;
			return;
		}
		const auto now = ::GetTickCount64();
		if (!Puppet::Active() || now - g_lastRead < 500) {
			return;
		}
		g_lastRead = now;
		const auto r = Read(a_f);
		if (!r.valid) {
			g_stats = {};
			if (r.why != g_lastWhy) {
				g_lastWhy = r.why;
				Log::Warn("progression: can't read the Tarnished ({}) -- Snake's damage stays MGSV's own (x1)", r.why);
			}
		} else {
			g_lastWhy.clear();
			g_stats.level = r.level;
			g_stats.upgrade = r.upgrade;
			g_stats.maxUpgrade = r.maxUpgrade;
			if (!r.weaponWhy.empty() && r.weaponWhy != g_lastWeaponWhy) {
				Log::Warn("progression: the right-hand armament can't be read ({}) -- its part of the factor is x1", r.weaponWhy);
			}
			g_lastWeaponWhy = r.weaponWhy;
		}
		const float factor = combat::ProgressionFactor(rules, g_stats);
		if (std::fabs(factor - g_factor) > 0.005f || r.level != g_lastLevel || r.weaponId != g_lastWeapon) {
			Log::Info("progression ({}): rune level {}, right hand {} (+{} of {}) -> Snake's damage x{:.2f}", combat::Name(rules.mode), g_stats.level, r.weaponId,
				g_stats.upgrade, g_stats.maxUpgrade, factor);
			g_lastLevel = r.level;
			g_lastWeapon = r.weaponId;
		}
		g_factor = factor;
	}

	float Factor()
	{
		return combat::ProgressionModeOf(Cfg().progression) == combat::ProgressionMode::kOff ? 1.0f : g_factor;
	}

	std::string Command(const Frame& a_f)
	{
		const auto  r = Read(a_f);
		const auto  rules = Rules();
		std::string out = std::format("[Progression] damage_scaling = {} (level_reference {}, level_exponent {}, weapon_bonus {}, factor {}..{})",
			combat::Name(rules.mode), rules.levelReference, rules.levelExponent, rules.weaponBonus, rules.minFactor, rules.maxFactor);
		if (!r.valid) {
			return out + std::format("\nthe Tarnished can't be read: {}", r.why);
		}
		combat::ProgressionStats s{ r.level, r.upgrade, r.maxUpgrade };
		out += std::format("\nthe Tarnished: rune level {} (Vig {} Min {} End {} Str {} Dex {} Int {} Fai {} Arc {}), max HP {}", r.level, r.attributes[0],
			r.attributes[1], r.attributes[2], r.attributes[3], r.attributes[4], r.attributes[5], r.attributes[6], r.attributes[7], r.maxHp);
		out += std::format("\nright hand: slot {} weapon {} (row {}, reinforce type {}) +{} of {}{}", r.rightSlot, r.weaponId, combat::WeaponRowOf(r.weaponId),
			r.reinforceType, r.upgrade, r.maxUpgrade, r.weaponWhy.empty() ? "" : std::format(" -- {}", r.weaponWhy));
		out += std::format("\nfactors: level x{:.2f}, weapon x{:.2f}; this mode x{:.2f} (in use: x{:.2f})", combat::LevelFactor(r.level, rules),
			combat::WeaponFactor(r.upgrade, r.maxUpgrade, rules), combat::ProgressionFactor(rules, s), Factor());
		return out;
	}
}
