#include "HorseLogic.h"

#include <algorithm>
#include <cmath>

namespace phantom::guest::horse::plan
{
	namespace
	{
		bool Agrees(const Vec3& a_feet, const Vec3& a_skin, const PickParams& a_p)
		{
			return (a_feet - a_skin).LengthXZ() <= a_p.maxXZ && std::fabs(a_feet.y - a_skin.y) <= a_p.maxDy;
		}

		// Pooled location objects nobody uses stand at the origin (live, ActorProxy v1): never a candidate.
		bool AtOrigin(const Vec3& a_p) { return std::fabs(a_p.x) <= 1.0f && std::fabs(a_p.z) <= 1.0f; }
	}

	std::uintptr_t PickLocation(const std::vector<LocSample>& a_samples, const Vec3& a_horseRoot, const std::vector<std::uintptr_t>& a_exclude,
		const PickParams& a_params)
	{
		if (!a_horseRoot.Finite()) {
			return 0;
		}
		std::uintptr_t best = 0;
		float          bestD = a_params.maxXZ;
		for (const auto& s : a_samples) {
			if (!s.loc || !s.feet.Finite() || AtOrigin(s.feet) || std::ranges::find(a_exclude, s.loc) != a_exclude.end()) {
				continue;
			}
			if (!Agrees(s.feet, a_horseRoot, a_params)) {
				continue;
			}
			const float d = (s.feet - a_horseRoot).LengthXZ();
			if (d <= bestD) {
				bestD = d;
				best = s.loc;
			}
		}
		return best;
	}

	void Tracker::Reset()
	{
		m_loc = 0;
		m_confirmed = false;
		m_agreed = m_off = m_settle = 0;
		m_firstAgreedMs = 0;
	}

	void Tracker::Offer(std::uintptr_t a_loc, std::uint64_t a_nowMs)
	{
		Reset();
		m_loc = a_loc;
		(void)a_nowMs;
	}

	void Tracker::NoteShift() { m_settle = kSettleChecks; }

	Tracker::Verdict Tracker::Observe(const std::optional<Vec3>& a_feet, const std::optional<Vec3>& a_skin, std::uint64_t a_nowMs)
	{
		if (!m_loc) {
			return Verdict::kNone;
		}
		if (!a_feet || !a_feet->Finite()) {
			Reset();  // freed or reused as something else: the vtable check failed
			return Verdict::kLost;
		}
		if (!a_skin || !a_skin->Finite()) {
			return m_confirmed ? Verdict::kConfirmed : Verdict::kCandidate;
		}
		if (Agrees(*a_feet, *a_skin, m_params)) {
			m_off = 0;
			if (m_settle > 0) {
				m_settle = 0;  // the skin caught up with the placement
			}
			if (m_agreed++ == 0) {
				m_firstAgreedMs = a_nowMs;
			}
			if (!m_confirmed && m_agreed >= kConfirmSamples && a_nowMs - m_firstAgreedMs >= kConfirmMs) {
				m_confirmed = true;
			}
			return m_confirmed ? Verdict::kConfirmed : Verdict::kCandidate;
		}
		if (m_settle > 0) {
			--m_settle;
			return m_confirmed ? Verdict::kConfirmed : Verdict::kCandidate;
		}
		if (!m_confirmed) {
			Reset();  // a candidate that ever leaves the skin was never the horse's
			return Verdict::kLost;
		}
		if (++m_off >= kLoseSamples) {
			Reset();
			return Verdict::kLost;
		}
		return Verdict::kConfirmed;
	}

	std::optional<Placement> Shifted(const Vec3& a_at, const std::array<float, 4>& a_rot, const Vec3& a_shift, float a_fallbackYaw)
	{
		if (!a_at.Finite() || !a_shift.Finite()) {
			return std::nullopt;
		}
		Placement p;
		p.pos = a_at + a_shift;
		const float n2 = a_rot[0] * a_rot[0] + a_rot[1] * a_rot[1] + a_rot[2] * a_rot[2] + a_rot[3] * a_rot[3];
		if (std::isfinite(n2) && std::fabs(n2 - 1.0f) <= 0.02f) {
			p.rot = a_rot;
		} else if (std::isfinite(a_fallbackYaw)) {
			p.rot = logic::YawQuat(a_fallbackYaw);
		} else {
			return std::nullopt;
		}
		return p;
	}

	Vec3 GroundedShift(const Vec3& a_shift, const Vec3& a_feet, float a_groundY) { return { a_shift.x, a_groundY - a_feet.y, a_shift.z }; }

	bool RiddenJumpOk(const Vec3& a_root, const Vec3& a_centre, float a_radius)
	{
		return a_root.Finite() && a_centre.Finite() && (a_root - a_centre).LengthXZ() <= RiddenJumpLimit(a_radius);
	}

	bool Followed(const Vec3& a_before, const Vec3& a_after, const Vec3& a_shift, float a_tolerance)
	{
		if (!a_before.Finite() || !a_after.Finite() || !a_shift.Finite()) {
			return false;
		}
		const Vec3 want = a_before + Vec3{ a_shift.x, 0.0f, a_shift.z };
		return (a_after - want).LengthXZ() <= a_tolerance;
	}

	Action Decide(const ActionInput& a_in)
	{
		if (!a_in.horse) {
			return Action::kLeave;
		}
		const bool movable = a_in.enabled && a_in.located && !a_in.unmovable;
		if (a_in.ridden) {
			return movable && !a_in.riderLeft ? Action::kMoveRidden : Action::kNoTreadmill;
		}
		if (!a_in.posValid) {
			return Action::kLeave;  // nowhere known to move it from; Follow.cpp whistles it when it shows up
		}
		return movable ? Action::kMoveAlong : Action::kWhistle;
	}
}
