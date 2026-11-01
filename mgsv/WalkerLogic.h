#pragma once
// D-Walker led alongside a moving Snake, the pure half (docs/GUEST.md 18.3 "Led alongside"; game glue in Walker.cpp, tests in
// tests/test_guest_horse.cpp -- the location tracker's home).
//
// Why: MGSV's walker AI takes an order 1-2 s after it is given, walks ~6.5 m/s and stops within ~3 m of a moving player, so
// a running Snake left it 4.8-5 m behind on average (median of 60 s of running, walking and stops; p90 8.4 m) whatever the
// order, spot or timing (GUEST.md 18.3, "Lag"). D-Walker owns a location object of the class Snake's belongs to (live
// 2026-10-31: the scan's object standing exactly on GetPosition), and MGSV's own SetPosition for the walker (0x14170D410) is that
// object's setter (vf 0x68) plus a request slot. So, while Snake moves, the walker is *led*: a point that tracks the follow spot
// beside him (his velocity fed forward, the gap closed with a time constant, never faster than maxSpeed: no frame moves it more
// than a few centimetres) and the walker placed there every frame through its own setter; its AI keeps walking toward an order
// ahead of him (Follow.cpp), so its legs move. When he stops the lead lets go and the walker is MGSV's again.

#include "Logic.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace phantom::guest::walker::plan
{
	using logic::Vec3;

	struct LeadSettings
	{
		float         side{ -2.8f };     // m, + = right along the camera's view of Snake (negative: the screen's left, as the follow spot)
		float         ahead{ 0.0f };     // m along that view (+ = further from the camera)
		float         tau{ 0.35f };      // s: the time constant the lead point closes its gap to the spot with (Snake's velocity fed forward)
		float         maxSpeed{ 9.0f };  // m/s: the lead never moves faster (0.15 m per frame at 60 fps)
		float         acquire{ 6.0f };   // m: a walker further from the spot than this is left to MGSV (it walks over, Follow's catch-up)
		float         startSpeed{ 1.5f };// m/s: Snake faster than this starts the lead
		float         stopSpeed{ 0.6f }; // m/s: ... and slower than this for stopMs ends it (the walker settles at the spot meanwhile)
		std::uint64_t stopMs{ 500 };
		float         turnRate{ 6.0f };  // rad/s: the walker turned toward the lead's motion at most this fast (0 = its own facing kept)
		float         lostAt{ 2.0f };    // m: the walker this far from where it was put (and not by us) = someone else moved it
	};

	struct LeadInput
	{
		bool          allowed{ false };  // Normal Mode, following on, on foot, nothing busy, the walker's object confirmed
		float         dt{ 0.0f };        // s since the last frame
		std::uint64_t nowMs{ 0 };
		Vec3          snake;             // his root (MGSV world)
		Vec3          velocity;          // his smoothed velocity (m/s; the odometer's)
		float         viewYaw{ 0.0f };   // the camera's view of him (Follow.cpp ViewYaw), radians
		Vec3          walker;            // the walker's feet now (its location object +0x10)
		float         walkerYaw{ 0.0f }; // its facing now
	};

	struct LeadOutput
	{
		bool  place{ false };  // put the walker at (to.x, its own height, to.z) facing yaw this frame
		Vec3  to;
		float yaw{ 0.0f };
		bool  started{ false };
		bool  stopped{ false };
		bool  lost{ false };   // stopped because the walker wasn't where we put it (moved by MGSV / a whistle / a warp)
	};

	inline float WrapPi(float a_a)
	{
		constexpr float kPi = 3.14159265358979f;
		while (a_a > kPi) {
			a_a -= 2.0f * kPi;
		}
		while (a_a < -kPi) {
			a_a += 2.0f * kPi;
		}
		return a_a;
	}

	// Where the walker should run: beside Snake as the camera sees him (as Follow.cpp's spot).
	inline Vec3 LeadSpot(const LeadInput& a_in, const LeadSettings& a_s) { return logic::BesideOf(a_in.snake, a_in.viewYaw, a_s.ahead, a_s.side); }

	class Lead
	{
	public:
		LeadOutput Update(const LeadInput& a_in, const LeadSettings& a_s)
		{
			LeadOutput out;
			const bool dtOk = a_in.dt > 0.0f && a_in.dt < 0.1f;
			if (!a_in.allowed || !dtOk || !a_in.snake.Finite() || !a_in.walker.Finite() || !a_in.velocity.Finite()) {
				out.stopped = m_active;
				m_active = false;
				return out;
			}
			const float speed = a_in.velocity.LengthXZ();
			const Vec3  spot = LeadSpot(a_in, a_s);
			if (!m_active) {
				if (speed < a_s.startSpeed || (a_in.walker - spot).LengthXZ() > a_s.acquire) {
					return out;
				}
				m_active = true;
				m_point = a_in.walker;
				m_yaw = a_in.walkerYaw;
				m_slowSince = 0;
				out.started = true;
			} else if ((a_in.walker - m_point).LengthXZ() > a_s.lostAt) {
				// Not where we put it last frame: MGSV moved it (a whistle's placement, a warp, its own planner) -- it is MGSV's again.
				m_active = false;
				out.stopped = out.lost = true;
				return out;
			}
			if (speed < a_s.stopSpeed) {
				if (!m_slowSince) {
					m_slowSince = a_in.nowMs ? a_in.nowMs : 1;
				} else if (a_in.nowMs - m_slowSince >= a_s.stopMs) {
					m_active = false;
					out.stopped = true;
					return out;
				}
			} else {
				m_slowSince = 0;
			}
			// Snake's own motion fed forward, the gap to the spot closed with tau, the step capped.
			const float k = a_s.tau > 0.0f ? 1.0f - std::exp(-a_in.dt / a_s.tau) : 1.0f;
			Vec3        step{ a_in.velocity.x * a_in.dt + (spot.x - m_point.x) * k, 0.0f, a_in.velocity.z * a_in.dt + (spot.z - m_point.z) * k };
			const float len = step.LengthXZ();
			const float cap = std::max(0.0f, a_s.maxSpeed) * a_in.dt;
			if (len > cap && len > 0.0f) {
				step = step * (cap / len);
			}
			m_point = { m_point.x + step.x, a_in.walker.y, m_point.z + step.z };
			// Facing: toward where it goes, turned at most turnRate (a walker sliding sideways looks wrong; one spinning too).
			if (a_s.turnRate > 0.0f && step.LengthXZ() > 0.2f * a_in.dt) {
				const float want = std::atan2(step.x, step.z);
				const float d = WrapPi(want - m_yaw);
				const float maxTurn = a_s.turnRate * a_in.dt;
				m_yaw = WrapPi(m_yaw + std::clamp(d, -maxTurn, maxTurn));
			}
			out.place = true;
			out.to = m_point;
			out.yaw = a_s.turnRate > 0.0f ? m_yaw : a_in.walkerYaw;
			return out;
		}

		// The treadmill moved Snake and the walker by a_shift in the same frame: the lead point goes with them.
		void Shift(const Vec3& a_shift)
		{
			if (m_active) {
				m_point = { m_point.x + a_shift.x, m_point.y + a_shift.y, m_point.z + a_shift.z };
			}
		}

		void                Stop() { m_active = false; }
		[[nodiscard]] bool  Active() const { return m_active; }
		[[nodiscard]] Vec3  Point() const { return m_point; }

	private:
		bool          m_active{ false };
		Vec3          m_point;
		float         m_yaw{ 0.0f };
		std::uint64_t m_slowSince{ 0 };
	};
}
