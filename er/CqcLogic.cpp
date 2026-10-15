#include "CqcLogic.h"

#include <algorithm>
#include <array>
#include <numbers>

namespace phantom::er::cqc
{
	std::optional<std::size_t> SelectTarget(const Vec3& a_snake, const Vec3& a_facing, const std::vector<Candidate>& a_candidates, const Rules& a_r)
	{
		const float                half = a_r.arcDeg * std::numbers::pi_v<float> / 180.0f;
		std::optional<std::size_t> best;
		float                      bestD = 1e9f;
		for (std::size_t i = 0; i < a_candidates.size(); ++i) {
			const auto& c = a_candidates[i];
			if (!c.hostile || !combat::InArc(a_snake, a_facing, a_r.range, half, c.cap)) {
				continue;
			}
			const float d = combat::CapsuleDistance(c.cap, a_snake);
			if (d < bestD) {
				bestD = d;
				best = i;
			}
		}
		return best;
	}

	bool IsBoss(std::int32_t a_maxHp, const combat::Scaling& a_s, const Rules& a_r)
	{
		if (a_maxHp <= 0 || !(a_s.playerMaxHp > 0.0f)) {
			return true;  // unknown (before Combat's first tick read the Tarnished): stagger only, never a frozen boss
		}
		if (a_r.bossHp > 0 && a_maxHp >= a_r.bossHp) {
			return true;
		}
		return combat::Bigness(a_maxHp, a_s) >= a_r.bossBigness;
	}

	bool NeedsHold(proto::CqcAction a_action) { return a_action == proto::kCqcChoke || a_action == proto::kCqcInterrogate || a_action == proto::kCqcHold; }

	Response Respond(proto::CqcAction a_action, const Situation& a_s)
	{
		switch (a_action) {
		case proto::kCqcGrab:
			return a_s.boss ? Response::kStagger : a_s.cooling && !a_s.held ? Response::kFlinch : Response::kHold;
		case proto::kCqcHold:
			return Response::kNone;  // MGSV's hold goes on: nothing new (the grab did it)
		case proto::kCqcThrow:
			return a_s.boss ? Response::kStagger : Response::kKnockdown;
		case proto::kCqcChoke:
			return !a_s.held ? Response::kNone : a_s.boss ? Response::kStagger : Response::kSleep;
		case proto::kCqcInterrogate:
			return a_s.held ? Response::kGag : Response::kNone;
		case proto::kCqcRelease:
			return Response::kRelease;
		}
		return Response::kNone;
	}

	std::string_view Name(Response a_r)
	{
		switch (a_r) {
		case Response::kNone:
			return "nothing";
		case Response::kHold:
			return "hold";
		case Response::kKnockdown:
			return "knockdown";
		case Response::kSleep:
			return "sleep";
		case Response::kGag:
			return "interrogation";
		case Response::kRelease:
			return "release";
		case Response::kStagger:
			return "stagger";
		case Response::kFlinch:
			return "flinch (hold cooldown)";
		case Response::kCount:
			break;
		}
		return "?";
	}

	std::string_view Name(proto::CqcAction a_action)
	{
		static constexpr std::array<std::string_view, 6> kNames{ "grab", "hold", "throw", "choke", "interrogate", "release" };
		return a_action < kNames.size() ? kNames[a_action] : std::string_view("?");
	}

	combat::HitEffect EffectOf(Response a_resp, std::int32_t a_maxHp, const combat::MeleeValues& a_melee, const combat::HitRules& a_hit, const Rules& a_r)
	{
		combat::HitEffect e;
		e.nonLethal = true;
		switch (a_resp) {
		case Response::kHold:
		case Response::kFlinch:
			e.poise = a_r.grabPoise;
			e.dmgLevel = a_r.grabDmgLevel;
			break;
		case Response::kKnockdown:
			e.poise = a_r.throwPoise;
			e.dmgLevel = a_r.throwDmgLevel;
			break;
		case Response::kStagger:
			e.poise = a_r.staggerPoise;
			e.dmgLevel = a_r.staggerDmgLevel;
			break;
		case Response::kSleep:
			e = combat::MeleeEffect(proto::kMeleeChoke, a_maxHp, a_melee, a_hit);
			e.damage = 0;
			break;
		default:
			return {};
		}
		if (a_resp != Response::kSleep && a_r.hitCorrection > 0) {
			// Some of Elden Ring's own hit for the reaction, a token of ours so Combat keeps the correction (a damage-free
			// non-lethal hit always flies with sleep_correction).
			e.damage = 1;
			e.correction = std::min<std::uint16_t>(a_r.hitCorrection, 100);
		}
		return e;
	}

	void Cooldowns::Start(std::uint64_t a_handle, std::uint64_t a_nowMs, float a_seconds)
	{
		const auto until = a_nowMs + static_cast<std::uint64_t>(std::max(0.0f, a_seconds) * 1000.0f);
		for (auto& [h, u] : m_until) {
			if (h == a_handle) {
				u = until;
				return;
			}
		}
		m_until.emplace_back(a_handle, until);
	}

	bool Cooldowns::Cooling(std::uint64_t a_handle, std::uint64_t a_nowMs) const
	{
		for (const auto& [h, u] : m_until) {
			if (h == a_handle) {
				return a_nowMs < u;
			}
		}
		return false;
	}

	void Cooldowns::Expire(std::uint64_t a_nowMs)
	{
		std::erase_if(m_until, [&](const auto& a_kv) { return a_nowMs >= a_kv.second; });
	}

	Wait WhenToFire(std::uint64_t a_nowMs, std::uint64_t a_askedMs, std::uint32_t a_settleMs, std::uint32_t a_deadlineMs, bool a_rowBusy)
	{
		const auto age = a_nowMs >= a_askedMs ? a_nowMs - a_askedMs : 0;
		if (age < a_settleMs) {
			return Wait::kWait;
		}
		if (!a_rowBusy) {
			return Wait::kFire;
		}
		return age >= static_cast<std::uint64_t>(a_settleMs) + a_deadlineMs ? Wait::kFireLate : Wait::kWait;
	}

	namespace
	{
		constexpr std::array<std::string_view, 8> kGags{
			"\"I'll talk! The Elden Ring... it's broken. Everyone knows that!\"",
			"\"There's a grace up ahead. Don't tell anyone I told you.\"",
			"\"Runes? I spent them all. Honest.\"",
			"\"I don't know any Venom Snake. Is he maidenless too?\"",
			"\"Try finger but hole.\"",
			"\"The Tarnished? He went that way. No -- the other way.\"",
			"\"Please... I have a Spirit Ash at home.\"",
			"\"Ammunition? We fight with swords here, you lunatic.\"",
		};
	}

	std::string_view GagLine(std::uint32_t a_index) { return kGags[a_index % kGags.size()]; }
	std::size_t      GagCount() { return kGags.size(); }
}
