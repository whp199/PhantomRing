#include "DogLogic.h"

#include "Config.h"
#include "DecoyLogic.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <numbers>

namespace phantom::er::dog
{
	Rules MakeRules(const DogConfig& a_c)
	{
		Rules r;
		r.spotRange = a_c.spotRange;
		r.spotHeight = a_c.spotHeight;
		r.spotLinger = a_c.spotLinger;
		r.spotTextEvery = a_c.spotTextEvery;
		r.targetRange = a_c.targetRange;
		r.targetMin = a_c.targetMin;
		r.targetConeDeg = a_c.targetConeDeg;
		r.markedFirst = a_c.targetMarkedFirst;
		r.clearRun = a_c.clearRun;
		r.runStepMax = a_c.runStepMax;
		r.bossStagger = a_c.bosses == "stagger";
		r.busyHold = a_c.busyHold;
		r.busyFlinchEvery = a_c.busyFlinchEvery;
		r.killShareMult = a_c.killShareMult;
		r.killShareFallback = a_c.killShareFallback;
		r.woundShare = a_c.woundShare;
		r.knockoutSleep = a_c.knockoutSleep;
		r.barkRange = a_c.barkRange;
		r.barkStare = a_c.barkStare;
		r.barkCooldown = a_c.barkCooldown;
		return r;
	}

	const char* Name(Why a_why)
	{
		static constexpr std::array<const char*, static_cast<std::size_t>(Why::kCount)> kNames{ "ok", "not hostile", "dead", "not rendered",
			"on a Fulton balloon", "a boss", "held (CQC)", "awake (Get it takes sleepers)", "too far", "too near (CQC's)", "off the centre", "out of sight",
			"no clear run", "nobody around" };
		const auto i = static_cast<std::size_t>(a_why);
		return i < kNames.size() ? kNames[i] : "?";
	}

	const char* Name(Run a_run)
	{
		switch (a_run) {
		case Run::kClear:
			return "clear";
		case Run::kStep:
			return "a step";
		case Run::kDrop:
			return "a drop";
		case Run::kWall:
			return "a wall";
		}
		return "?";
	}

	namespace
	{
		// The angle between the view's axis and the nearest part of a body of a_radius around a_aim (as marking::OffAxis).
		float OffAxis(const View& a_view, const Vec3& a_aim, float a_radius)
		{
			const Vec3  d = a_aim - a_view.pos;
			const float dist = Length(d);
			if (!(dist > std::max(a_radius, 1e-3f))) {
				return 0.0f;  // the camera is inside him
			}
			const float fl = Length(a_view.fwd);
			if (!(fl > 1e-4f) || !Finite(d)) {
				return std::numbers::pi_v<float>;
			}
			const float along = Dot(d, a_view.fwd) / fl;
			if (!(along > 0.0f)) {
				return std::numbers::pi_v<float>;
			}
			const float angle = std::acos(std::clamp(along / dist, -1.0f, 1.0f));
			const float body = std::asin(std::clamp(a_radius / dist, 0.0f, 1.0f));
			return std::max(0.0f, angle - body);
		}
	}

	Why Screen(const Candidate& a_c, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_r, float* a_off)
	{
		const float off = OffAxis(a_view, a_c.aim, std::clamp(a_c.radius, 0.0f, 3.0f));
		if (a_off) {
			*a_off = off;
		}
		// The order matters only for what is reported: nothing gets past the first two whatever else holds.
		if (!a_c.hostile) {
			return Why::kNotHostile;
		}
		if (!a_c.alive) {
			return Why::kDead;
		}
		if (!a_c.rendered) {
			return Why::kHidden;
		}
		if (a_c.lifting) {
			return Why::kLifting;
		}
		if (a_c.boss && !a_r.bossStagger) {
			return Why::kBoss;
		}
		if (a_c.held) {
			return Why::kHeld;
		}
		if (a_fetch && !a_c.asleep) {
			return Why::kAwake;
		}
		const float d = Length(a_c.feet - a_snake);
		if (!std::isfinite(d) || d > a_r.targetRange) {
			return Why::kTooFar;
		}
		if (d < a_r.targetMin) {
			return Why::kTooNear;
		}
		if (off > a_r.targetConeDeg * std::numbers::pi_v<float> / 180.0f) {
			return Why::kOffCentre;
		}
		return Why::kOk;
	}

	bool PointsAtAttack(std::int16_t a_rx, std::int16_t a_ry)
	{
		const float x = static_cast<float>(a_rx) / 32767.0f;
		const float y = static_cast<float>(a_ry) / 32767.0f;
		return std::hypot(x, y) > 0.5f && x > 0.0f && std::fabs(y) <= x * std::tan(35.0f * std::numbers::pi_v<float> / 180.0f);
	}

	bool LatchFits(const Candidate& a_c, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_r)
	{
		Rules wide = a_r;
		wide.targetConeDeg = std::min(90.0f, a_r.targetConeDeg * kLatchConeMult);
		return Screen(a_c, a_view, a_snake, a_fetch, wide) == Why::kOk;
	}

	bool LatchKeeps(const Candidate& a_c, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_r, const RunFn& a_run)
	{
		if (!LatchFits(a_c, a_view, a_snake, a_fetch, a_r)) {
			return false;
		}
		return !a_r.clearRun || !a_run || a_run(a_c) == Run::kClear;
	}

	std::uint64_t PickLatch::Update(std::uint64_t a_fresh, bool a_fits, std::uint64_t a_nowMs, std::uint64_t a_latchMs)
	{
		held_ = false;
		if (a_fresh && a_fresh == handle_) {
			freshAt_ = a_nowMs;
			return handle_;
		}
		if (handle_ && a_fits && a_nowMs >= freshAt_ && a_nowMs - freshAt_ < a_latchMs) {
			held_ = true;
			return handle_;
		}
		handle_ = a_fresh;
		freshAt_ = a_fresh ? a_nowMs : 0;
		return handle_;
	}

	std::vector<Vec3> RunPoints(const Vec3& a_dog, const Vec3& a_target, float a_stopShort)
	{
		std::vector<Vec3> out;
		if (!Finite(a_dog) || !Finite(a_target)) {
			return out;
		}
		const Vec3  d = a_target - a_dog;
		const float flat = LengthXZ(d);
		const float len = std::max(0.0f, flat - std::max(0.0f, a_stopShort));
		out.push_back(a_dog);
		if (!(flat > 1e-3f) || !(len > 1e-3f)) {
			return out;
		}
		const int n = std::max(1, static_cast<int>(std::ceil(len / kRunSpacing)));
		for (int i = 1; i <= n; ++i) {
			const float t = len * static_cast<float>(i) / static_cast<float>(n) / flat;  // share of the whole dog -> target line
			out.push_back(a_dog + d * t);
		}
		return out;
	}

	Run ClearRun(std::span<const std::optional<float>> a_ground, bool a_wall, float a_stepMax)
	{
		if (a_wall) {
			return Run::kWall;
		}
		for (std::size_t i = 0; i < a_ground.size(); ++i) {
			if (!a_ground[i] || !std::isfinite(*a_ground[i])) {
				return Run::kDrop;
			}
			if (i > 0 && std::fabs(*a_ground[i] - *a_ground[i - 1]) > std::max(0.0f, a_stepMax)) {
				return Run::kStep;
			}
		}
		return Run::kClear;
	}

	PickResult Pick(std::span<const Candidate> a_cands, const View& a_view, const Vec3& a_snake, bool a_fetch, const Rules& a_r, const SightFn& a_sight,
		const RunFn& a_run, std::size_t a_maxChecks)
	{
		PickResult out;
		struct Ranked
		{
			std::size_t i;
			bool        marked;
			float       off;
		};
		std::vector<Ranked> ok;
		float               bestRefusedOff = 1e9f;
		bool                anyHostile = false;
		for (std::size_t i = 0; i < a_cands.size(); ++i) {
			float      off = 0;
			const auto why = Screen(a_cands[i], a_view, a_snake, a_fetch, a_r, &off);
			if (why == Why::kOk) {
				ok.push_back({ i, a_r.markedFirst && a_cands[i].marked, off });
				continue;
			}
			if (why == Why::kNotHostile) {
				continue;  // never reported as "the one you look at": he isn't a candidate at all
			}
			anyHostile = true;
			if (off < bestRefusedOff) {
				bestRefusedOff = off;
				out.why = why;
				out.nearest = i;
				out.nearestOff = off;
			}
		}
		if (!anyHostile && ok.empty()) {
			out.why = a_cands.empty() ? Why::kNobody : Why::kNotHostile;
		}
		std::sort(ok.begin(), ok.end(), [](const Ranked& a, const Ranked& b) { return a.marked != b.marked ? a.marked : a.off < b.off; });
		// The checked ones in the order they are preferred: the first one refused by its rays is the one reported (a candidate the
		// rules let through says more than a nearer one they didn't).
		bool reported = false;
		for (const auto& r : ok) {
			if (out.checked >= a_maxChecks) {
				break;
			}
			++out.checked;
			const auto& c = a_cands[r.i];
			if (a_sight && !a_sight(c)) {
				if (!reported) {
					reported = true;
					out.why = Why::kNoSight;
					out.nearest = r.i;
					out.nearestOff = r.off;
				}
				continue;
			}
			if (a_r.clearRun && a_run) {
				const Run run = a_run(c);
				if (run != Run::kClear) {
					if (!reported) {
						reported = true;
						out.why = Why::kNoRun;
						out.run = run;
						out.nearest = r.i;
						out.nearestOff = r.off;
					}
					continue;
				}
			}
			out.index = r.i;
			out.why = Why::kOk;
			out.nearest = r.i;
			out.nearestOff = r.off;
			return out;
		}
		return out;
	}

	std::string NoPickText(const PickResult& a_r, const Rules& a_rules)
	{
		const std::string look = std::format("D-Dog: look at an enemy ({:.0f} m)", a_rules.targetRange);
		if (a_r.index || !a_r.nearest || !(a_r.nearestOff <= 2.0f * a_rules.targetConeDeg * std::numbers::pi_v<float> / 180.0f)) {
			return look;
		}
		switch (a_r.why) {
		case Why::kNoRun:
			return std::format("D-Dog can't run to him straight ({} in the way)", Name(a_r.run));
		case Why::kNoSight:
			return "D-Dog: he is out of sight";
		case Why::kTooNear:
			return "D-Dog: he is too close -- CQC him";
		case Why::kTooFar:
			return std::format("D-Dog: he is too far (over {:.0f} m)", a_rules.targetRange);
		case Why::kBoss:
			return "D-Dog won't take on a boss";
		case Why::kHeld:
			return "D-Dog: he is held already";
		case Why::kAwake:
			return "D-Dog: Get it needs a sleeping enemy";
		case Why::kLifting:
			return "D-Dog: a Fulton balloon has him";
		default:
			return look;
		}
	}

	// ---- spotting -------------------------------------------------------------------------------------------------------------
	std::vector<std::uint64_t> Spotter::Update(std::span<const Smelled> a_seen, const std::optional<Vec3>& a_dog, std::uint64_t a_now, const Rules& a_r)
	{
		std::vector<std::uint64_t> fresh;
		const auto linger = static_cast<std::uint64_t>(std::max(0.0f, a_r.spotLinger) * 1000.0f);
		for (const auto& s : a_seen) {
			auto it = std::ranges::find_if(until_, [&](const auto& a_kv) { return a_kv.first == s.handle; });
			if (!s.hostile || !s.alive) {
				if (it != until_.end()) {
					until_.erase(it);  // dead (or no longer an enemy): not counted; his mark fades on its own
				}
				continue;
			}
			if (!a_dog || !Finite(s.pos) || !s.rendered) {
				continue;
			}
			const Vec3 d = s.pos - *a_dog;
			if (!(LengthXZ(d) <= a_r.spotRange) || !(std::fabs(d.y) <= a_r.spotHeight)) {
				continue;
			}
			if (it == until_.end() || it->second <= a_now) {
				fresh.push_back(s.handle);
			}
			if (it == until_.end()) {
				until_.emplace_back(s.handle, a_now + linger);
			} else {
				it->second = a_now + linger;
			}
		}
		std::erase_if(until_, [&](const auto& a_kv) { return a_kv.second <= a_now; });
		return fresh;
	}

	bool Spotter::Spotted(std::uint64_t a_handle, std::uint64_t a_now) const { return Until(a_handle) > a_now; }

	std::uint64_t Spotter::Until(std::uint64_t a_handle) const
	{
		auto it = std::ranges::find_if(until_, [&](const auto& a_kv) { return a_kv.first == a_handle; });
		return it == until_.end() ? 0 : it->second;
	}

	std::size_t Spotter::Count(std::uint64_t a_now) const
	{
		return static_cast<std::size_t>(std::ranges::count_if(until_, [&](const auto& a_kv) { return a_kv.second > a_now; }));
	}

	std::string FoundText::Update(std::size_t a_fresh, std::size_t a_total, std::uint64_t a_now, float a_everyS)
	{
		pending_ = (pending_ || a_fresh > 0) && a_total > 0;
		const auto every = static_cast<std::uint64_t>(std::max(0.0f, a_everyS) * 1000.0f);
		if (!pending_ || (said_ && a_now - last_ < every)) {
			return {};
		}
		pending_ = false;
		said_ = true;
		last_ = a_now;
		return a_total > 1 ? std::format("D-Dog: Found enemy ({} nearby)", a_total) : std::string("D-Dog: Found enemy");
	}

	// ---- the bite's effect ----------------------------------------------------------------------------------------------------
	const char* Name(Act a_act)
	{
		static constexpr std::array<const char*, static_cast<std::size_t>(Act::kCount)> kNames{ "nothing", "hold", "flinch", "kill", "knock out", "wound",
			"fetch", "stagger (boss)", "refused (a boss)" };
		const auto i = static_cast<std::size_t>(a_act);
		return i < kNames.size() ? kNames[i] : "?";
	}

	namespace
	{
		std::int32_t Lives(float a_lives, std::int32_t a_maxHp, const combat::HitRules& a_hit)
		{
			if (!(a_lives > 0.0f) || a_maxHp <= 0) {
				return 0;
			}
			const float progression = std::isfinite(a_hit.progression) && a_hit.progression > 0.0f ? a_hit.progression : 1.0f;
			const float v = a_lives * combat::ReferenceHp(a_maxHp, a_hit.scaling) * progression;
			return std::clamp(static_cast<std::int32_t>(std::lround(v)), 1, std::max(1, a_hit.maxDamage));
		}

		// The knockdown that comes with kill, knock out and wound: CQC's throw (its poise and dmgLevel).
		void Knockdown(combat::HitEffect& a_e, const cqc::Rules& a_cqc)
		{
			a_e.poise = a_cqc.throwPoise;
			a_e.dmgLevel = a_cqc.throwDmgLevel;
		}
	}

	combat::HitEffect Flinch(std::int32_t a_maxHp, const combat::HitRules& a_hit, const cqc::Rules& a_cqc, const combat::MeleeValues& a_melee)
	{
		return cqc::EffectOf(cqc::Response::kHold, a_maxHp, a_melee, a_hit, a_cqc);
	}

	Effect EffectOf(proto::DogStyle a_style, std::int32_t a_maxHp, float a_share, bool a_boss, bool a_holdRefused, bool a_bossCooling, const Rules& a_r,
		const combat::HitRules& a_hit, const cqc::Rules& a_cqc, const combat::MeleeValues& a_melee)
	{
		Effect e;
		if (a_boss) {
			// Never a boss pinned, killed, slept or wounded by the dog: bosses = off refuses him outright, stagger gives one stagger.
			if (!a_r.bossStagger) {
				e.act = Act::kBossRefused;
				return e;
			}
			if (a_bossCooling || a_style == proto::kDogFetch) {
				return e;
			}
			e.act = Act::kStagger;
			e.hit = cqc::EffectOf(cqc::Response::kStagger, a_maxHp, a_melee, a_hit, a_cqc);
			return e;
		}
		switch (a_style) {
		case proto::kDogBusy:
		default:  // an unknown style: the gentlest
			e.act = a_holdRefused ? Act::kFlinch : Act::kHold;
			e.hit = Flinch(a_maxHp, a_hit, a_cqc, a_melee);
			e.holdSeconds = a_holdRefused ? 0.0f : std::max(0.0f, a_r.busyHold);
			return e;
		case proto::kDogFetch:
			e.act = Act::kFetch;
			return e;
		case proto::kDogKill: {
			e.act = Act::kKill;
			const bool  shared = std::isfinite(a_share) && a_share > 0.0f;
			const float lives = shared ? std::min(a_share, 20.0f) * std::max(0.0f, a_r.killShareMult) : std::max(0.0f, a_r.killShareFallback);
			e.hit.damage = Lives(lives, a_maxHp, a_hit);
			e.hit.shared = shared;
			e.hit.landOrHp = true;  // MGSV's knife connected: a missed bullet still deals it (Combat's HP route)
			Knockdown(e.hit, a_cqc);
			return e;
		}
		case proto::kDogKnockOut:
			e.act = Act::kKnockOut;
			// The stun knife: the sleep darts' route (rungs of the sleep ladder on the hit bullet), knockout_sleep of his resistance
			// (the choke's absolute build-up when it can't be read), with the knockdown's token hit.
			e.hit.sleep = a_melee.chokeSleep;
			e.hit.sleepShare = std::max(0.0f, a_r.knockoutSleep);
			Knockdown(e.hit, a_cqc);
			e.hit.nonLethal = true;
			if (a_cqc.hitCorrection > 0) {
				e.hit.damage = 1;
				e.hit.correction = std::min<std::uint16_t>(a_cqc.hitCorrection, 100);
			}
			return e;
		case proto::kDogWound:
			e.act = Act::kWound;
			// "Critically wound": wound_share of his reference HP (1.0: all of an ordinary enemy's), never the killing blow -- he is
			// left at 1 HP at least (combat: nonLethal).
			e.hit.damage = Lives(std::max(0.0f, a_r.woundShare), a_maxHp, a_hit);
			e.hit.nonLethal = true;
			e.hit.landOrHp = true;
			Knockdown(e.hit, a_cqc);
			return e;
		}
	}

	bool QuietEnd(std::uint32_t a_lastCommand, std::uint64_t a_msSince, bool a_live)
	{
		return !a_live || ((a_lastCommand == 8 || a_lastCommand == 9 || a_lastCommand == 16) && a_msSince < kQuietEndMs);
	}

	std::optional<proto::DogStyle> StyleOfCommand(std::uint32_t a_command)
	{
		switch (a_command) {
		case 11:
			return proto::kDogBusy;
		case 12:
			return proto::kDogKill;
		case 13:
			return proto::kDogKnockOut;
		case 14:
			return proto::kDogWound;
		case 15:
			return proto::kDogFetch;
		default:
			return std::nullopt;
		}
	}

	const char* StyleName(std::uint32_t a_style)
	{
		static constexpr std::array<const char*, 5> kNames{ "busy", "fetch", "kill", "knock out", "wound" };
		return a_style < kNames.size() ? kNames[a_style] : "unknown";
	}

	const char* PhaseName(std::uint32_t a_phase)
	{
		static constexpr std::array<const char*, 5> kNames{ "ordered", "connected", "released", "refused", "lost" };
		return a_phase < kNames.size() ? kNames[a_phase] : "?";
	}

	// ---- Bark ------------------------------------------------------------------------------------------------------------------
	bool HearsBark(const Listener& a_l, const Vec3& a_dogChest, const Rules& a_r)
	{
		if (!a_l.hostile || !decoy::Fooled(a_l.asleep, a_l.boss) || a_l.held || a_l.lifting) {
			return false;
		}
		decoy::Rules d;
		d.range = 0.0f;  // heard, not seen: no cone, no ray
		d.activeRange = std::max(0.0f, a_r.barkRange);
		d.snakeClose = a_r.barkSnakeClose;
		return decoy::Notices(a_l.eye, a_l.forward, a_dogChest, decoy::Variant::kActive, a_l.toSnake, d) == decoy::Notice::kHears;
	}
}
