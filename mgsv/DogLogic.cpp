// D-Dog, the pure half (DogLogic.h; tests in tests/test_guest_dog.cpp).
#include "DogLogic.h"

#include <algorithm>
#include <cmath>

namespace phantom::guest::dog::plan
{
	proto::DogStyle StyleOf(int a_byte)
	{
		switch (a_byte) {
		case 0:
			return proto::kDogBusy;
		case 1:
			return proto::kDogFetch;
		case 2:
			return proto::kDogKnockOut;  // the stun knife (SNEAKING (STUN)): "Do it (knock out enemy)", command 13 (live 2026-11-04)
		case 3:
			return proto::kDogWound;     // the battledress: "Do it (wound enemy)", command 14 (live)
		case 4:
			return proto::kDogKill;      // the knife (SNEAKING (KNIFE)): "Do it (kill enemy)", command 12 (live 2026-11-04)
		default:
			// 5-7: attacks whose mode the attack state sends as the bite; anything else (or < 0): unknown.
			return a_byte >= 5 && a_byte <= 7 ? proto::kDogBusy : proto::kDogStyleUnknown;
		}
	}

	std::uint32_t AttackIdOf(proto::DogStyle a_style)
	{
		switch (a_style) {
		case proto::kDogBusy:
			return 144;  // ATK_DDogBite
		case proto::kDogKill:
			return 145;  // ATK_DDogKnife
		case proto::kDogKnockOut:
			return 146;  // ATK_DDogStunKnife
		case proto::kDogWound:
			return 147;  // ATK_DDogBodyAttack
		default:
			return 0;
		}
	}

	std::optional<proto::DogStyle> StyleOfCommand(int a_command)
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

	std::optional<int> CommandOfStyle(proto::DogStyle a_style)
	{
		for (int c = kCmdAttackFirst; c <= kCmdAttackLast; ++c) {
			if (StyleOfCommand(c) == a_style) {
				return c;
			}
		}
		return std::nullopt;
	}

	std::uint32_t AttackIdOfMessage(std::uint64_t a_message, bool a_stun)
	{
		switch (a_message) {
		case 0x887890EBE04Aull:
			return 144;
		case 0x53E70021556Aull:
			return a_stun ? 146 : 145;
		case 0x48E4EDE3DAEDull:
			return 147;
		default:
			return 0;
		}
	}

	const char* StyleName(proto::DogStyle a_style)
	{
		switch (a_style) {
		case proto::kDogBusy:
			return "busy";
		case proto::kDogFetch:
			return "fetch";
		case proto::kDogKill:
			return "kill";
		case proto::kDogKnockOut:
			return "knock out";
		case proto::kDogWound:
			return "wound";
		default:
			return "unknown";
		}
	}

	const char* PhaseName(proto::DogPhase a_phase)
	{
		switch (a_phase) {
		case proto::kDogOrdered:
			return "Ordered";
		case proto::kDogConnected:
			return "Connected";
		case proto::kDogReleased:
			return "Released";
		case proto::kDogRefused:
			return "Refused";
		case proto::kDogLost:
			return "Lost";
		}
		return "?";
	}

	const char* StageName(Stage a_stage)
	{
		switch (a_stage) {
		case Stage::kIdle:
			return "idle";
		case Stage::kRealize:
			return "realizing its soldier";
		case Stage::kOrdered:
			return "ordered";
		case Stage::kConnected:
			return "connected";
		}
		return "?";
	}

	JobStep Job::End(proto::DogPhase a_phase, bool a_release, const char* a_why)
	{
		JobStep s;
		s.event = a_phase;
		s.release = a_release;
		// Whistled back after anything that sent it running (Refused before an order never did: only the "no target" case whistles,
		// so the dog that would have run at MGSV's own target stays by Snake).
		s.whistle = true;
		s.why = a_why;
		m_stage = Stage::kIdle;
		return s;
	}

	JobStep Job::Start(std::uint64_t a_pick, proto::DogStyle a_style, std::uint64_t a_nowMs, bool a_waiting)
	{
		m_handle = a_pick;
		m_style = a_style;
		m_startedMs = a_nowMs;
		m_orderedMs = m_connectedMs = 0;
		m_best = -1.0f;
		m_bestMs = a_nowMs;
		if (!a_pick) {
			m_stage = Stage::kIdle;
			JobStep s;
			s.event = proto::kDogRefused;
			s.whistle = !a_waiting;
			s.why = a_waiting ? "no Elden Ring enemy picked (kActorDogTarget); the dog keeps waiting" : "no Elden Ring enemy picked (kActorDogTarget)";
			return s;
		}
		m_stage = Stage::kRealize;
		JobStep s;
		s.dedicate = true;
		s.why = "a soldier stands in for the target";
		return s;
	}

	JobStep Job::Abort(const char* a_why)
	{
		switch (m_stage) {
		case Stage::kIdle:
			return {};
		case Stage::kRealize:
			return End(proto::kDogRefused, true, a_why);
		case Stage::kOrdered:
			return End(proto::kDogLost, true, a_why);
		case Stage::kConnected:
			return End(proto::kDogReleased, true, a_why);
		}
		return {};
	}

	JobStep Job::Update(const JobInput& a_in, const JobSettings& a_s)
	{
		switch (m_stage) {
		case Stage::kIdle:
			return {};
		case Stage::kRealize:
			if (a_in.cancel) {
				auto s = End(proto::kDogRefused, true, "the player called the dog back");
				s.whistle = !PlayerPlacesDog(a_in.cancelCommand);
				return s;
			}
			if (!a_in.targetPresent) {
				return End(proto::kDogRefused, true, "the target left the table (dead or gone)");
			}
			if (a_in.proxyReady) {
				m_stage = Stage::kOrdered;
				m_orderedMs = a_in.nowMs;
				m_best = a_in.dogDistance;
				m_bestMs = a_in.nowMs;
				JobStep s;
				s.event = proto::kDogOrdered;
				s.order = true;
				s.why = "its soldier stands at the target";
				return s;
			}
			if (a_in.nowMs - m_startedMs >= a_s.realizeTimeoutMs) {
				return End(proto::kDogRefused, true, "its soldier wasn't realized in time");
			}
			return {};
		case Stage::kOrdered: {
			if (a_in.accepted || a_in.fultoned) {
				m_stage = Stage::kConnected;
				m_connectedMs = a_in.nowMs;
				JobStep s;
				s.event = proto::kDogConnected;
				s.why = a_in.fultoned ? "MGSV's balloon took the target's soldier" : "the soldier accepted the dog's attack";
				return s;
			}
			if (a_in.cancel) {
				auto s = End(proto::kDogLost, true, "the player called the dog back before it connected");
				s.whistle = !PlayerPlacesDog(a_in.cancelCommand);
				return s;
			}
			if (a_in.orderDroppedMs >= (m_style == proto::kDogFetch ? a_s.fetchDropLostMs : a_s.dropLostMs)) {
				return End(proto::kDogLost, true, m_style == proto::kDogFetch ? "MGSV let go of the sleeper and no Fulton followed" : "MGSV gave the order up");
			}
			if (!a_in.targetPresent) {
				return End(proto::kDogLost, true, "the target left the table (dead or gone) before the dog connected");
			}
			const bool fetch = m_style == proto::kDogFetch;
			if (a_in.nowMs - m_orderedMs >= (fetch ? std::max(a_s.reachTimeoutMs, a_s.fetchTimeoutMs) : a_s.reachTimeoutMs)) {
				return End(proto::kDogLost, true, fetch ? "no Fulton within the fetch timeout" : "no bite within the reach timeout");
			}
			// Stuck: once it had time to set off, the distance hasn't shrunk by stuckGain for stuckMs while it is still far.
			if (a_in.dogDistance >= 0.0f) {
				if (m_best < 0.0f || a_in.dogDistance < m_best - a_s.stuckGain || a_in.nowMs - m_orderedMs < a_s.stuckGraceMs) {
					if (m_best < 0.0f || a_in.dogDistance < m_best - a_s.stuckGain) {
						m_best = a_in.dogDistance;
					}
					m_bestMs = a_in.nowMs;
				} else if (const float near = fetch ? a_s.fetchNear : a_s.stuckNear;
				           m_best > near && a_in.dogDistance > near && a_in.nowMs - m_bestMs >= a_s.stuckMs) {
					// (never once it has been at its target: a fetching dog leaves the sleeper once its balloon is fixed, live)
					return End(proto::kDogLost, true, "the dog stopped getting nearer (stuck)");
				}
			}
			return {};
		}
		case Stage::kConnected:
			if (a_in.cancel) {
				auto s = End(proto::kDogReleased, true, "the player called the dog back");
				s.whistle = !PlayerPlacesDog(a_in.cancelCommand);
				return s;
			}
			if (!a_in.targetPresent) {
				return End(proto::kDogReleased, true, "the target is dead or gone");
			}
			if (a_in.contactOver && a_in.nowMs - m_connectedMs >= a_s.contactGraceMs) {
				return End(proto::kDogReleased, true, "the dog let go");
			}
			if (a_in.nowMs - m_connectedMs >= a_s.busyMaxMs) {
				return End(proto::kDogReleased, true, "the busy time ran out");
			}
			return {};
		}
		return {};
	}

	bool MoveAlong(const TreadmillInput& a_in)
	{
		if (!a_in.enabled || !a_in.located || !std::isfinite(a_in.distance)) {
			return false;
		}
		if (a_in.engaged) {
			return a_in.distance <= std::max(a_in.maxDistance, a_in.waitMax);
		}
		return a_in.distance <= (a_in.waiting ? std::max(a_in.maxDistance, a_in.waitMax) : a_in.maxDistance);
	}

	View ViewOf(const Vec3& a_cam, const Vec3& a_snake)
	{
		View        v;
		const Vec3  d = a_snake - a_cam;
		const float back = d.LengthXZ();
		if (!(back >= 0.5f) || !a_cam.Finite() || !a_snake.Finite()) {
			return v;
		}
		v.valid = true;
		v.cam = a_cam;
		v.dir = { d.x / back, 0.0f, d.z / back };
		v.back = back;
		return v;
	}

	bool InView(const View& a_view, const Vec3& a_p)
	{
		if (!a_view.valid) {
			return true;  // unknown: assume seen (nothing is moved)
		}
		const float dx = a_p.x - a_view.cam.x, dz = a_p.z - a_view.cam.z;
		const float depth = dx * a_view.dir.x + dz * a_view.dir.z;
		const float lateral = std::fabs(dx * a_view.dir.z - dz * a_view.dir.x);
		return depth > -1.0f && lateral < std::max(0.0f, depth) * 0.7f + 2.0f;
	}

	std::optional<Vec3> LeashSpot(const Vec3& a_snake, const Vec3& a_velocity, const View& a_view, const Vec3& a_dog, float a_leash, float a_side)
	{
		const float gap = (a_dog - a_snake).LengthXZ();
		if (!a_view.valid || !(gap > a_leash) || InView(a_view, a_dog)) {
			return std::nullopt;
		}
		const float viewYaw = std::atan2(a_view.dir.x, a_view.dir.z);
		Vec3        to = logic::BesideOf(a_snake, viewYaw, -2.5f, a_side);
		if (InView(a_view, to)) {
			const float speed = a_velocity.LengthXZ();
			const Vec3  motion = speed > 0.3f ? Vec3{ a_velocity.x / speed, 0.0f, a_velocity.z / speed } : a_view.dir;
			const float back = a_view.back + 1.5f;
			to = { a_snake.x - motion.x * back, a_snake.y, a_snake.z - motion.z * back };
		}
		if (InView(a_view, to) || (to - a_snake).LengthXZ() >= gap - 1.0f) {
			return std::nullopt;
		}
		to.y = a_snake.y;
		return to;
	}

	Vec3 DisplayLimiter::Step(const Vec3& a_real, float a_dt, bool a_reset, const DisplaySettings& a_s)
	{
		if (!a_real.Finite()) {
			return a_real;
		}
		if (a_reset || !m_have || !(a_dt > 0.0f)) {
			m_have = true;
			m_shown = a_real;
			m_offset = {};
			return a_real;
		}
		const Vec3  d{ a_real.x - m_shown.x, 0.0f, a_real.z - m_shown.z };
		const float gap = d.LengthXZ();
		const float step = a_s.maxSpeed * std::clamp(a_dt, 1.0f / 240.0f, 0.1f);
		if (gap > a_s.teleport) {
			++m_teleports;
			m_shown = a_real;
		} else if (gap > step) {
			++m_limited;
			m_shown = { m_shown.x + d.x * (step / gap), a_real.y, m_shown.z + d.z * (step / gap) };
		} else {
			m_shown = a_real;
		}
		m_shown.y = a_real.y;
		m_offset = { m_shown.x - a_real.x, 0.0f, m_shown.z - a_real.z };
		m_maxLag = std::max(m_maxLag, m_offset.LengthXZ());
		return m_shown;
	}

	Vec3 WaitFade::Step(const Vec3& a_offset, const std::optional<Vec3>& a_dog, const Goal& a_goal, float a_dt)
	{
		const float len = a_offset.LengthXZ();
		if (!(len > 0.001f) || !a_offset.Finite() || a_goal.kind == kHolding) {
			*this = {};
			return {};
		}
		const float dt = std::clamp(a_dt, 0.0f, 0.1f);
		const float arrive = a_goal.kind == kSoldier ? 1.0f : 3.0f;
		const auto  way = [&]() { return a_dog && a_dog->Finite() ? std::optional<float>((*a_dog - a_goal.at).LengthXZ()) : std::nullopt; };
		if (!m_have || a_goal.kind != m_kind) {
			m_have = true;
			m_kind = a_goal.kind;
			m_from = a_offset;
			m_way = way().value_or(0.0f);
			m_frac = 1.0f;
		}
		if (a_goal.kind == kNone) {
			return a_offset;
		}
		const auto shrink = [&](float a_speed) {
			const float step = a_speed * dt;
			return len <= step ? Vec3{} : a_offset * ((len - step) / len);
		};
		const auto w = way();
		if (!w || m_way <= arrive + 0.5f) {
			return shrink(4.0f);
		}
		m_frac = std::min(m_frac, std::clamp((*w - arrive) / (m_way - arrive), 0.0f, 1.0f));
		const Vec3 byWay = m_from * m_frac;
		const Vec3 byTime = shrink(0.5f);
		return byWay.LengthXZ() < byTime.LengthXZ() ? byWay : byTime;
	}

	Vec3 StagedSpot(const Vec3& a_root, float a_yaw, float a_behind) { return logic::BesideOf(a_root, a_yaw, -a_behind, 0.0f); }

	bool IsMgsvMarkerColor(float a_r, float a_g, float a_b, float a_tol)
	{
		const float m = std::max({ a_r, a_g, a_b });
		if (!(m > 0.03f) || !std::isfinite(m)) {
			return false;
		}
		const float r = a_r / m, g = a_g / m, b = a_b / m;
		const auto  near = [&](float a_x, float a_y, float a_z) { return std::fabs(r - a_x) <= a_tol && std::fabs(g - a_y) <= a_tol && std::fabs(b - a_z) <= a_tol; };
		return near(0.4f / 0.9f, 1.0f, 0.7f / 0.9f) || near(1.0f, 0.4f, 0.3f);
	}
}
