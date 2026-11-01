#include "ClimbLogic.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace phantom::guest::climb::plan
{
	namespace
	{
		constexpr float kDeg = std::numbers::pi_v<float> / 180.0f;
		constexpr std::int16_t kStickUp = 32767, kStickDown = -32767;
		// A root jump this big in one frame is one of our placements landing, not climbing.
		constexpr float kPlacementJump = 1.0f;
		// Off the ladder (NORMAL_ACTION back) this long while climbing: he climbed over the top / stepped off the foot -- mounted
		// again (the loop).
		constexpr std::uint64_t kOffLadderMs = 60;
		constexpr int           kMountTries = 3;
		// A climb-up taken from a vault site ends once his root has risen this much and starts down again.
		constexpr float kCutRise = 0.5f;
		// Hanging on the ladder his root is within this of the climbing line (horizontal); at the bottom stand point he is 0.35 m
		// off it.
		constexpr float kOnLine = 0.2f;

		// The way home (kReturn), once its placement went out at a_placedAt: is he back on the sandbox?
		enum class Home
		{
			kWait,   // not yet (the placement lands over a frame or two)
			kHome,
			kAgain,  // not there kHomeWaitMs after it: another try
			kWarp,   // kReturnTries placements and still not there: MGSV's Warp
		};
		Home CheckHome(const Facts& a_f, const Vec3& a_home, std::uint64_t a_placedAt, int a_tries)
		{
			if ((a_f.root - a_home).LengthXZ() < kHomeRadius) {
				return Home::kHome;
			}
			if (a_f.nowMs - a_placedAt < kHomeWaitMs) {
				return Home::kWait;
			}
			return a_tries < kReturnTries ? Home::kAgain : Home::kWarp;
		}

		bool AtHome(const Facts& a_f, const Vec3& a_home) { return (a_f.root - a_home).LengthXZ() < kHomeRadius; }
	}

	Vec3 Facing(const LadderSite& a_site)
	{
		const float yaw = a_site.yawDeg * kDeg;
		return { std::sin(yaw), 0.0f, std::cos(yaw) };
	}

	Vec3 BottomStand(const LadderSite& a_site, const LadderGeometry& a_geo) { return a_site.origin + Facing(a_site) * a_geo.bottomOut; }

	Vec3 TopStand(const LadderSite& a_site, const LadderGeometry& a_geo)
	{
		Vec3 p = a_site.origin - Facing(a_site) * a_geo.topIn;
		p.y += a_geo.topHeight;
		return p;
	}

	float LineDistance(const LadderSite& a_site, const LadderGeometry& a_geo, const Vec3& a_root)
	{
		return (a_root - (a_site.origin + Facing(a_site) * a_geo.lineOut)).LengthXZ();
	}

	float BottomFacing(const LadderSite& a_site) { return (a_site.yawDeg + 180.0f) * kDeg; }
	float TopFacing(const LadderSite& a_site) { return a_site.yawDeg * kDeg; }

	float YawError(float a_yaw, float a_want)
	{
		const float tau = 2.0f * std::numbers::pi_v<float>;
		float       d = std::fmod(std::fabs(a_yaw - a_want), tau);
		return d > std::numbers::pi_v<float> ? tau - d : d;
	}

	bool ReadyAtStand(const Facts& a_f, std::uint64_t a_settleMs, const Vec3& a_stand, float a_want, std::uint64_t& a_standSince, int& a_retries,
		Step& a_s)
	{
		if (!(a_f.normalAction && a_f.standing)) {
			a_standSince = 0;  // still getting up (a stance change is no NORMAL_ACTION either)
			return false;
		}
		if (!a_standSince) {
			a_standSince = a_f.nowMs;
		}
		if (a_f.nowMs - a_standSince < a_settleMs) {
			return false;
		}
		if (YawError(a_f.yaw, a_want) > 25.0f * kDeg && a_retries < 3) {
			++a_retries;
			a_standSince = 0;
			a_s.place = a_stand;
			a_s.placeYaw = a_want;
			a_s.face = true;
			a_s.note = "not facing it: turned again";
			return false;
		}
		return true;
	}

	int RecycleShift(bool a_down, int a_rung, int a_target, int a_top)
	{
		// The record must look like one of MGSV's ladders long enough to move along: the span fits between the two thresholds.
		const int high = a_top - 2;
		if (a_rung < 0 || a_target < 0 || a_top < 0 || a_top > 256 || high - kRecycleSpan <= kRecycleLow) {
			return 0;
		}
		if (!a_down && std::max(a_rung, a_target) >= high && std::min(a_rung, a_target) - kRecycleSpan >= kRecycleLow) {
			return -kRecycleSpan;
		}
		if (a_down && std::min(a_rung, a_target) <= kRecycleLow && std::max(a_rung, a_target) + kRecycleSpan <= high) {
			return kRecycleSpan;
		}
		return 0;
	}

	// ---- Pacer ------------------------------------------------------------------------------------
	void Pacer::Start(float a_speed)
	{
		speed_ = a_speed;
		owed_ = 2.0f * kAhead;  // he starts at once
	}

	void Pacer::SetSpeed(float a_speed) { speed_ = a_speed; }

	bool Pacer::Push(double a_dt, float a_climbed)
	{
		if (std::isfinite(speed_) && speed_ == 0.0f) {
			owed_ = 0.0f;
			return false;  // Elden Ring's climber stopped on the rungs: so does MGSV's
		}
		if (!std::isfinite(speed_) || speed_ < 0.0f) {
			owed_ = 0.0f;
			return true;  // unknown: MGSV's own pace
		}
		const float dt = std::isfinite(a_dt) ? static_cast<float>(std::clamp(a_dt, 0.0, 0.25)) : 0.0f;
		const float climbed = std::isfinite(a_climbed) ? a_climbed : 0.0f;
		owed_ = std::clamp(owed_ + speed_ * dt - climbed, -kMaxAhead, kMaxOwed);
		return owed_ > kAhead;
	}

	const char* PhaseName(Phase a_phase)
	{
		switch (a_phase) {
		case Phase::kIdle:
			return "idle";
		case Phase::kPlace:
			return "placed at the stand point";
		case Phase::kMount:
			return "action pressed";
		case Phase::kMove:
			return "moving";
		case Phase::kLeave:
			return "leaving the ladder";
		case Phase::kReturn:
			return "back to the sandbox";
		}
		return "?";
	}

	// ---- Ladder -----------------------------------------------------------------------------------
	void Ladder::Start(const LadderSite& a_site, const LadderGeometry& a_geo, bool a_down, float a_speed, const Vec3& a_home, float a_homeYaw,
		std::uint64_t a_nowMs)
	{
		site_ = a_site;
		geo_ = a_geo;
		down_ = a_down;
		stopAsked_ = false;
		retried_ = false;
		home_ = a_home;
		homeYaw_ = a_homeYaw;
		lastY_.reset();
		climbed_ = 0.0f;
		loops_ = 0;
		recycles_ = 0;
		noRecycle_ = false;
		mounts_ = 0;
		droppedOff_ = false;
		leaveTried_ = false;
		mountTries_ = 0;
		offSince_ = 0;
		mountedMs_ = 0;
		standSince_ = 0;
		faceRetries_ = 0;
		returnTries_ = 0;
		placeFails_ = 0;
		pacer_.Start(a_speed);
		phase_ = Phase::kPlace;
		since_ = a_nowMs;
		placed_ = false;
	}

	void Ladder::Change(bool a_down, float a_speed)
	{
		down_ = a_down;
		pacer_.Start(a_speed);
	}

	void Ladder::Stop(std::uint64_t a_nowMs)
	{
		(void)a_nowMs;
		if (phase_ != Phase::kIdle) {
			stopAsked_ = true;
		}
	}

	void Ladder::PlacementFailed(std::uint64_t a_nowMs)
	{
		++placeFails_;
		if (phase_ == Phase::kReturn) {
			placed_ = false;  // home again next frame (kReturnTries in all, then MGSV's Warp)
		} else if (phase_ != Phase::kIdle) {
			Enter(Phase::kReturn, a_nowMs);  // at MGSV's ladder (or at its first stand placement, still home): home from where he is
		}
	}

	Step Ladder::Enter(Phase a_phase, std::uint64_t a_nowMs)
	{
		phase_ = a_phase;
		since_ = a_nowMs;
		placed_ = false;
		standSince_ = 0;
		Step s;
		s.active = true;
		return s;
	}

	Step Ladder::Update(const Facts& a_f, double a_dt)
	{
		Step s;
		if (phase_ == Phase::kIdle) {
			return s;
		}
		s.active = true;
		if (!a_f.ready) {
			return s;  // MGSV's menu, a load: nothing moves until it plays again
		}
		const auto now = a_f.nowMs;
		const Vec3 stand = down_ ? TopStand(site_, geo_) : BottomStand(site_, geo_);
		const float facing = down_ ? TopFacing(site_) : BottomFacing(site_);
		switch (phase_) {
		case Phase::kPlace:
			if (!placed_) {
				placed_ = true;
				since_ = now;
				s.place = stand;
				s.placeYaw = facing;
				s.face = true;  // facing the ladder (from the roof: its top), as a player walks up to it
				return s;
			}
			if (!a_f.normalAction && LineDistance(site_, geo_, a_f.root) < kOnLine && now - since_ >= 100) {
				// On the ladder already: placed at its top facing the hole MGSV took him onto it by itself (live, the loop down)
				s = Enter(Phase::kMount, now);
				phase_ = Phase::kMount;
				return Update(a_f, a_dt);
			}
			if (stopAsked_) {
				s = Enter(Phase::kReturn, now);
				s.note = "stopped before the mount";
				return s;
			}
			if (ReadyAtStand(a_f, timing.settleMs, stand, facing, standSince_, faceRetries_, s)) {
				s = Enter(Phase::kMount, now);
				s.pressAction = true;
			} else if (now - since_ >= 3 * timing.returnMs) {
				s = Enter(Phase::kReturn, now);
				s.note = "never stood at the ladder";
			}
			return s;
		case Phase::kMount:
			if (!a_f.normalAction && LineDistance(site_, geo_, a_f.root) < kOnLine) {
				mountedMs_ = now - since_;
				phase_ = Phase::kMove;
				since_ = now;
				lastY_ = a_f.root.y;
				offSince_ = 0;
				++mounts_;
				s.note = mounts_ == 1 ? "on the ladder" : nullptr;
				if (stopAsked_) {
					s = Enter(Phase::kLeave, now);
				}
				return s;
			}
			if (stopAsked_ && a_f.normalAction) {
				s = Enter(Phase::kReturn, now);
				s.note = "stopped before the mount";
				return s;
			}
			if (now - since_ >= timing.mountMs) {
				if (++mountTries_ < kMountTries) {
					// Y unanswered (he slid into cover against the wall, or a stance change ate it, live): placed and faced again.
					s = Enter(Phase::kPlace, now);
					s.note = "no mount yet: placed at the ladder again";
				} else {
					s = Enter(Phase::kReturn, now);
					s.note = "the ladder never took him";
				}
			}
			return s;
		case Phase::kMove:
			{
				if (stopAsked_) {
					return Enter(Phase::kLeave, now);
				}
				float climbedNow = 0.0f;
				if (lastY_) {
					const float dy = a_f.root.y - *lastY_;
					if (std::fabs(dy) <= kPlacementJump) {
						climbedNow = down_ ? -dy : dy;
						climbed_ += std::max(0.0f, climbedNow);
					}
				}
				lastY_ = a_f.root.y;
				// Seamless: still on the ladder, near one of its ends -- moved along it (Climb.cpp: ladder_rung::Shift), his root jumps
				// kRecycleSpan * 0.5 m next frame (more than kPlacementJump: not counted as climbed).
				if (!noRecycle_ && !a_f.normalAction) {
					if (const int shift = RecycleShift(down_, a_f.rung, a_f.rungTarget, a_f.rungTop)) {
						s.rungShift = shift;
						++recycles_;
					}
				}
				// The loop: MGSV's 8 m ladder ends (he climbs over the top / steps off the foot, as any climber does) and he is mounted
				// again from where he climbs from. MGSV moves him along its ladder by no placement (live: a native one never, its Warp
				// only in a state not found again), so the loop is MGSV's own end and mount.
				if (a_f.normalAction) {
					if (!offSince_) {
						offSince_ = now;
					}
					if (now - offSince_ >= kOffLadderMs) {
						s = Enter(Phase::kPlace, now);
						mountTries_ = 0;
						faceRetries_ = 0;
						++loops_;
						return s;
					}
				} else {
					offSince_ = 0;
				}
				const bool push = pacer_.Push(a_dt, climbedNow);
				s.stickY = push ? (down_ ? kStickDown : kStickUp) : std::int16_t(0);
				return s;
			}
		case Phase::kLeave:
			if (!placed_) {
				// Off the ladder at once: a direct-motion request (played and stopped) ends MGSV's ladder action -- he lets go and
				// drops (live) -- and he is placed home as soon as it has (no placement reaches him on the ladder).
				placed_ = true;
				since_ = now;
				leaveY_ = a_f.root.y;
				s.knockOff = true;
				droppedOff_ = true;
				return s;
			}
			if (a_f.normalAction || a_f.root.y < leaveY_ - 0.15f || LineDistance(site_, geo_, a_f.root) > 0.3f) {
				return Enter(Phase::kReturn, now);
			}
			if (now - since_ >= timing.leaveMaxMs) {
				s = Enter(Phase::kReturn, now);
				s.note = "still on the ladder: placed back anyway";
				return s;
			}
			if (now - since_ >= timing.leaveMs) {
				s.stickY = kStickDown;  // the request didn't take: climb down off it
			}
			return s;
		case Phase::kReturn:
			if (!placed_ && !droppedOff_ && !leaveTried_ && !a_f.normalAction && LineDistance(site_, geo_, a_f.root) < 0.3f) {
				leaveTried_ = true;  // still on the ladder (no placement reaches him there): off it first
				return Enter(Phase::kLeave, now);
			}
			if (!placed_) {
				if (placeFails_ > 0 && AtHome(a_f, home_)) {
					phase_ = Phase::kIdle;  // a placement was refused, but he is home anyway (it was the first one, at the ladder)
					s.active = false;
					s.note = "back on the sandbox";
					return s;
				}
				if (placeFails_ >= kReturnTries) {
					phase_ = Phase::kIdle;
					s.active = false;
					s.warpHome = true;
					s.note = "the placement home was refused every time: MGSV's Warp home";
					return s;
				}
				placed_ = true;
				since_ = now;
				++returnTries_;
				s.place = home_;
				s.placeYaw = homeYaw_;
				s.face = true;
				return s;
			}
			if (droppedOff_) {
				// The drop off the ladder was a fall: MGSV would play its landing (~1 s crouched) at home. The same request
				// again, right after the placement, cancels it: he stands (live 2026-10-31).
				droppedOff_ = false;
				s.knockOff = true;
			}
			switch (CheckHome(a_f, home_, since_, returnTries_)) {
			case Home::kWait:
				return s;
			case Home::kAgain:
				// Not on the sandbox: still on MGSV's ladder (kLeave's way off it didn't take: no placement reaches him there) --
				// off it again first -- or the placement didn't land: home again.
				s = Enter(!a_f.normalAction && LineDistance(site_, geo_, a_f.root) < 0.3f ? Phase::kLeave : Phase::kReturn, now);
				s.note = "not back on the sandbox: home again";
				return s;
			case Home::kWarp:
				phase_ = Phase::kIdle;
				s.active = false;
				s.warpHome = true;
				s.note = "never got back on the sandbox: MGSV's Warp home";
				return s;
			case Home::kHome:
				break;
			}
			if ((a_f.normalAction && now - since_ >= 100) || now - since_ >= timing.returnMs) {
				phase_ = Phase::kIdle;
				s.active = false;
				s.note = a_f.normalAction ? "back on the sandbox" : "back on the sandbox (not standing yet)";
			}
			return s;
		case Phase::kIdle:
			break;
		}
		return s;
	}

	// ---- Mantle -----------------------------------------------------------------------------------
	MantleSite PickMantle(int a_heightCm, bool a_vault, const MantleSite& a_vaultSite, const MantleSite& a_low, const MantleSite& a_high,
		int a_lowMaxCm, const MantleSite& a_step, int a_stepMaxCm)
	{
		if (a_vault) {
			return a_vaultSite;
		}
		// A low ledge: the sacks (75 cm), where MGSV's climb-up is a knee onto the top and he is up 0.2 s into it (the crate's: 0.4-0.5 s)
		if (a_step.valid && a_heightCm <= a_stepMaxCm) {
			return a_step;
		}
		if (a_low.valid && (a_heightCm <= a_lowMaxCm || !a_high.valid)) {
			return a_low;
		}
		if (a_high.valid) {
			return a_high;
		}
		// No climb-up site: the vault's first half is one -- he goes up onto the obstacle, hands on top, a knee up -- and the move
		// is cut at its top (MantleSite::cutAtTop), before he drops down its far side.
		MantleSite s = a_vaultSite;
		s.cutAtTop = true;
		return s;
	}

	void Mantle::Start(const MantleSite& a_site, const Vec3& a_home, float a_homeYaw, std::uint64_t a_nowMs)
	{
		cut_ = false;
		standSince_ = 0;
		faceRetries_ = 0;
		site_ = a_site;
		home_ = a_home;
		homeYaw_ = a_homeYaw;
		phase_ = Phase::kPlace;
		since_ = a_nowMs;
		placed_ = false;
		retried_ = false;
		started_ = false;
		rose_ = 0.0f;
		moveMs_ = 0;
		returnTries_ = 0;
		placeFails_ = 0;
	}

	void Mantle::PlacementFailed(std::uint64_t a_nowMs)
	{
		++placeFails_;
		if (phase_ == Phase::kReturn) {
			placed_ = false;  // home again next frame (kReturnTries in all, then MGSV's Warp)
		} else if (phase_ != Phase::kIdle) {
			Enter(Phase::kReturn, a_nowMs);
		}
	}

	Step Mantle::Enter(Phase a_phase, std::uint64_t a_nowMs)
	{
		standSince_ = 0;
		phase_ = a_phase;
		since_ = a_nowMs;
		placed_ = false;
		Step s;
		s.active = true;
		return s;
	}

	Step Mantle::Update(const Facts& a_f)
	{
		Step s;
		if (phase_ == Phase::kIdle) {
			return s;
		}
		s.active = true;
		if (!a_f.ready) {
			return s;
		}
		const auto now = a_f.nowMs;
		switch (phase_) {
		case Phase::kPlace:
			if (!placed_) {
				placed_ = true;
				since_ = now;
				s.place = site_.stand;
				s.placeYaw = site_.yawDeg * kDeg;
				s.face = true;  // the obstacle must be in front of him
				return s;
			}
			if (ReadyAtStand(a_f, timing.settleMs, site_.stand, site_.yawDeg * kDeg, standSince_, faceRetries_, s)) {
				s = Enter(Phase::kMount, now);
				s.pressAction = true;
				pressedAt_ = now;
				startY_ = a_f.root.y;
			} else if (now - since_ >= 3 * timing.returnMs) {
				s = Enter(Phase::kReturn, now);
				s.note = "never stood at the obstacle";
			}
			return s;
		case Phase::kMount:
			if (!a_f.normalAction) {
				started_ = true;
				phase_ = Phase::kMove;
				since_ = now;
				s.note = "in the move";
				return s;
			}
			if (now - since_ >= timing.startMs) {
				if (!retried_) {
					retried_ = true;
					since_ = now;
					pressedAt_ = now;
					s.pressAction = true;
					s.note = "no move yet: action again";
				} else {
					s = Enter(Phase::kReturn, now);
					s.note = "MGSV offered no move there";
				}
			}
			return s;
		case Phase::kMove:
			{
				const float rise = a_f.root.y - startY_;
				const bool  topped = site_.cutAtTop && rose_ >= kCutRise && rise < rose_ - 0.02f;  // past the top of the climb
				// A vault is over once he is down on the far side: MGSV keeps him out of NORMAL_ACTION ~0.7 s more (live: 2.2 s from Y
				// to NORMAL_ACTION, down on the far side at ~1.5 s), a recovery Elden Ring's own move doesn't have.
				const bool  landed = !site_.cutAtTop && rose_ >= kCutRise && rise <= 0.05f;
				rose_ = std::max(rose_, rise);
				if (topped || landed) {
					// MGSV's move isn't over yet (the far side's drop, its recovery): a direct-motion request ends it, as it ends the
					// ladder's, and once more after the placement home (no landing there).
					moveMs_ = now - pressedAt_;
					s = Enter(Phase::kReturn, now);
					s.knockOff = true;
					cut_ = true;
					s.note = topped ? "on top: cut there (a climb-up)" : "down on the far side";
					return s;
				}
			}
			if (a_f.normalAction || now - since_ >= timing.maxMoveMs) {
				moveMs_ = now - pressedAt_;
				s = Enter(Phase::kReturn, now);
				s.note = a_f.normalAction ? "move over" : "move still going: over anyway";
			}
			return s;
		case Phase::kReturn:
			if (!placed_) {
				if (placeFails_ > 0 && AtHome(a_f, home_)) {
					phase_ = Phase::kIdle;  // a placement was refused, but he is home anyway (it was the first one, at the obstacle)
					s.active = false;
					s.note = "back on the sandbox";
					return s;
				}
				if (placeFails_ >= kReturnTries) {
					phase_ = Phase::kIdle;
					s.active = false;
					s.warpHome = true;
					s.note = "the placement home was refused every time: MGSV's Warp home";
					return s;
				}
				placed_ = true;
				since_ = now;
				++returnTries_;
				s.place = home_;
				s.placeYaw = homeYaw_;
				s.face = true;
				return s;
			}
			if (cut_) {
				cut_ = false;
				s.knockOff = true;
			}
			switch (CheckHome(a_f, home_, since_, returnTries_)) {
			case Home::kWait:
				return s;
			case Home::kAgain:
				// Not on the sandbox (MGSV's move still held him where it plays): whatever it plays ended, and home again.
				s = Enter(Phase::kReturn, now);
				s.knockOff = !a_f.normalAction;
				s.note = "not back on the sandbox: home again";
				return s;
			case Home::kWarp:
				phase_ = Phase::kIdle;
				s.active = false;
				s.warpHome = true;
				s.note = "never got back on the sandbox: MGSV's Warp home";
				return s;
			case Home::kHome:
				break;
			}
			if ((a_f.normalAction && now - since_ >= 100) || now - since_ >= timing.returnMs) {
				phase_ = Phase::kIdle;
				s.active = false;
				s.note = "back on the sandbox";
			}
			return s;
		case Phase::kLeave:
		case Phase::kIdle:
			break;
		}
		return s;
	}
}
