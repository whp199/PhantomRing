#include "GraceLogic.h"

#include "Config.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace phantom::er::grace
{
	Rules MakeRules(const GraceConfig& a_c)
	{
		Rules r;
		r.rings.clear();
		for (const float x : a_c.seatRings) {
			if (std::isfinite(x) && x > 0.3f && x < 10.0f) {
				r.rings.push_back(x);
			}
		}
		if (r.rings.empty()) {
			r.rings = { 2.0f };
		}
		r.dirs = std::clamp(a_c.seatDirs, 4, 64);
		r.screenX = a_c.seatScreenX;
		r.menuX = a_c.seatMenuX;
		r.step = a_c.seatStep;
		r.slopeNy = a_c.seatSlopeNy;
		r.spread = a_c.seatSpread;
		r.headroom = a_c.seatHeadroom;
		r.snakeClear = a_c.seatSnakeClear;
		r.charClear = a_c.seatCharClear;
		r.blackWaitMs = a_c.seatBlackWaitMs;
		r.deadlineMs = a_c.seatDeadlineMs;
		r.lateEchoMs = a_c.seatLateEchoMs;
		r.reloadReadyMs = a_c.seatReloadReadyMs;
		return r;
	}

	// ---- the observed black ------------------------------------------------------------------------------------------------
	float FadeAlpha(const Plate& a_p, float* a_rgb)
	{
		const auto sane = [](float a_v, float a_lo, float a_hi) { return std::isfinite(a_v) && a_v >= a_lo && a_v <= a_hi; };
		for (int i = 0; i < 4; ++i) {
			if (!sane(a_p.from[i], -0.01f, 1.01f) || !sane(a_p.to[i], -0.01f, 1.01f)) {
				return NAN;
			}
		}
		if (!sane(a_p.length, 0.0f, 60.0f) || !sane(a_p.left, -0.1f, 60.0f)) {
			return NAN;
		}
		const float k = a_p.length > 1e-4f ? std::clamp(a_p.left / a_p.length, 0.0f, 1.0f) : 0.0f;
		if (a_rgb) {
			for (int i = 0; i < 3; ++i) {
				a_rgb[i] = a_p.to[i] + (a_p.from[i] - a_p.to[i]) * k;
			}
		}
		return std::clamp(a_p.to[3] + (a_p.from[3] - a_p.to[3]) * k, 0.0f, 1.0f);
	}

	bool PlateDark(const Plate& a_p)
	{
		float      rgb[3]{};
		const float a = FadeAlpha(a_p, rgb);
		return std::isfinite(a) && a >= kDarkAlpha && rgb[0] <= 0.05f && rgb[1] <= 0.05f && rgb[2] <= 0.05f;
	}

	void BlackWatch::Press(std::uint64_t a_nowMs)
	{
		*this = {};
		pressMs_ = a_nowMs ? a_nowMs : 1;
	}

	bool BlackWatch::Update(std::uint64_t a_nowMs, bool a_dark)
	{
		if (!pressMs_ || reported_) {
			return false;
		}
		const auto t = static_cast<std::int64_t>(a_nowMs - pressMs_);
		if (a_dark && !dark_) {
			++blacks_;
			if (startMs_ < 0) {
				startMs_ = t;
			}
		} else if (!a_dark && dark_) {
			lastMs_ = t;
			if (endMs_ < 0) {
				endMs_ = t;
			}
		}
		dark_ = a_dark;
		if ((endMs_ >= 0 && !dark_) || (startMs_ < 0 && a_nowMs - pressMs_ > kWindowMs)) {
			reported_ = true;
			return true;
		}
		return false;
	}

	// ---- the gate ----------------------------------------------------------------------------------------------------------
	void Gate::Rest(std::uint64_t a_nowMs)
	{
		if (phase_ == Phase::kShown) {
			return;  // a re-rest at the same grace (Y again after B): she stays where she sits
		}
		*this = {};
		phase_ = Phase::kArmed;
		pressMs_ = a_nowMs ? a_nowMs : 1;
	}

	Gate::Event Gate::Update(const In& a_in, const Rules& a_r)
	{
		if (phase_ == Phase::kIdle) {
			return Event::kNone;
		}
		if (!a_in.atGrace) {
			*this = {};
			return Event::kEnded;
		}
		dialog_ = a_in.dialog;
		if (a_in.dialog && (phase_ == Phase::kArmed || phase_ == Phase::kSearching || phase_ == Phase::kPublished || phase_ == Phase::kShown)) {
			phase_ = Phase::kCancelled;
			return Event::kCancelled;
		}
		switch (phase_) {
		case Phase::kArmed:
			if (a_in.dark) {
				blackMs_ = a_in.nowMs ? a_in.nowMs : 1;
				phase_ = Phase::kSearching;
				return Event::kBlack;
			}
			if (a_in.nowMs - pressMs_ > static_cast<std::uint64_t>(std::max(0, a_r.blackWaitMs))) {
				phase_ = Phase::kNoSeat;
				return Event::kNoBlack;
			}
			return Event::kNone;
		case Phase::kSearching:
			if (!a_in.dark) {
				phase_ = Phase::kNoSeat;
				return Event::kLight;
			}
			if (a_in.nowMs - blackMs_ > static_cast<std::uint64_t>(std::max(0, a_r.deadlineMs))) {
				phase_ = Phase::kNoSeat;
				return Event::kTooLate;
			}
			return Event::kNone;
		case Phase::kPublished:
			if (a_in.echo) {
				phase_ = Phase::kShown;
				shownMs_ = a_in.nowMs ? a_in.nowMs : 1;
				shownLate_ = !a_in.dark;  // the screen is back: faded in (SeatAlpha), never popped in
				return Event::kShown;
			}
			if (!a_in.dark && a_in.nowMs - publishMs_ > static_cast<std::uint64_t>(std::max(0, a_r.lateEchoMs))) {
				phase_ = Phase::kHidden;
				return Event::kLateEcho;
			}
			return Event::kNone;
		default:
			return Event::kNone;
		}
	}

	Gate::Event Gate::SearchDone(std::uint64_t a_nowMs, bool a_found, bool a_dark, const Rules& a_r)
	{
		if (phase_ != Phase::kSearching) {
			return Event::kNone;
		}
		if (!a_found) {
			phase_ = Phase::kNoSeat;
			return Event::kNoSpot;
		}
		if (!a_dark) {
			phase_ = Phase::kNoSeat;
			return Event::kLight;
		}
		if (a_nowMs - blackMs_ > static_cast<std::uint64_t>(std::max(0, a_r.deadlineMs))) {
			phase_ = Phase::kNoSeat;
			return Event::kTooLate;
		}
		phase_ = Phase::kPublished;
		publishMs_ = a_nowMs ? a_nowMs : 1;
		return Event::kPublish;
	}

	const char* Name(Gate::Phase a_p)
	{
		switch (a_p) {
		case Gate::Phase::kIdle: return "idle";
		case Gate::Phase::kArmed: return "armed (waiting for the black)";
		case Gate::Phase::kSearching: return "searching (in the black)";
		case Gate::Phase::kPublished: return "published (hidden until the echo)";
		case Gate::Phase::kShown: return "seated (shown)";
		case Gate::Phase::kHidden: return "hidden this rest (late echo)";
		case Gate::Phase::kNoSeat: return "no seat this rest";
		case Gate::Phase::kCancelled: return "cancelled (an Elden Ring dialog)";
		}
		return "?";
	}

	const char* Name(Gate::Event a_e)
	{
		switch (a_e) {
		case Gate::Event::kNone: return "none";
		case Gate::Event::kBlack: return "black";
		case Gate::Event::kNoBlack: return "no black (a talk?)";
		case Gate::Event::kPublish: return "publish";
		case Gate::Event::kTooLate: return "too late";
		case Gate::Event::kLight: return "the black ended first";
		case Gate::Event::kNoSpot: return "no seat found";
		case Gate::Event::kShown: return "shown";
		case Gate::Event::kLateEcho: return "late echo";
		case Gate::Event::kCancelled: return "cancelled";
		case Gate::Event::kEnded: return "ended";
		}
		return "?";
	}

	// ---- the centre --------------------------------------------------------------------------------------------------------
	std::optional<Vec3> NearestCentre(std::span<const Vec3> a_assets, const Vec3& a_player, float a_max)
	{
		std::optional<Vec3> best;
		float               bestD = a_max;
		for (const auto& a : a_assets) {
			if (!Finite(a)) {
				continue;
			}
			const float d = LengthXZ(a - a_player);
			if (d <= bestD && std::fabs(a.y - a_player.y) < 3.0f) {
				bestD = d;
				best = a;
			}
		}
		return best;
	}

	Vec3 FacingCentre(const Vec3& a_player, const Vec3& a_forward, float a_dist)
	{
		Vec3 f{ a_forward.x, 0, a_forward.z };
		f = LengthXZ(f) > 1e-4f ? Normalize(f) : Vec3{ 0, 0, 1 };
		return a_player + f * a_dist;
	}

	// ---- the seat ----------------------------------------------------------------------------------------------------------
	ScreenPoint Project(const View& a_v, const Vec3& a_p)
	{
		ScreenPoint s;
		const Vec3  d = a_p - a_v.pos;
		const float z = Dot(d, a_v.fwd);
		s.depth = z;
		if (!(z > 0.1f)) {
			return s;
		}
		const float t = std::tan(std::clamp(a_v.fovY, 0.05f, 3.0f) * 0.5f);
		const float aspect = a_v.aspect > 0.1f ? a_v.aspect : 16.0f / 9.0f;
		s.front = true;
		s.x = (Dot(d, a_v.right) / (z * t * aspect) + 1.0f) * 0.5f;
		s.y = (1.0f - Dot(d, a_v.up) / (z * t)) * 0.5f;
		return s;
	}

	const char* Name(Why a_w)
	{
		switch (a_w) {
		case Why::kOk: return "ok";
		case Why::kNoFloor: return "no floor";
		case Why::kStep: return "a step from the grace's floor";
		case Why::kSlope: return "too steep";
		case Why::kUneven: return "uneven footprint";
		case Why::kHeadroom: return "no headroom";
		case Why::kKnee: return "a wall or a ledge at the knees";
		case Why::kReach: return "not reachable from the grace";
		case Why::kOnSnake: return "on Snake";
		case Why::kCharacter: return "next to a character";
		case Why::kBehindView: return "behind the camera";
		case Why::kOffScreen: return "off screen";
		case Why::kUnderMenu: return "under the grace menu";
		case Why::kViewBlocked: return "the camera can't see it";
		case Why::kBehindSnake: return "behind Snake";
		case Why::kCoversSnake: return "in front of Snake";
		case Why::kCount: break;
		}
		return "?";
	}

	float Score(float a_r, float a_screenX, float a_dh, const Rules& a_rules)
	{
		const float sx = std::isfinite(a_screenX) ? std::fabs(a_screenX - a_rules.screenX) : 0.0f;
		return std::fabs(a_r - 2.0f) + 2.0f * sx + 0.5f * std::fabs(a_dh);
	}

	namespace
	{
		constexpr float kTwoPi = 2.0f * std::numbers::pi_v<float>;

		float ScreenXOf(const Scene& a_s, const Vec3& a_feet, const Rules& a_r, bool* a_front = nullptr)
		{
			if (!a_s.view.valid) {
				if (a_front) {
					*a_front = true;
				}
				return NAN;
			}
			const auto p = Project(a_s.view, a_feet + Vec3{ 0, a_r.bodyHeight, 0 });
			if (a_front) {
				*a_front = p.front;
			}
			return p.front ? p.x : NAN;
		}

		// A line between two points is clear: no hit, or one within 0.3 m of its end.
		bool LineClear(const Vec3& a_from, const Vec3& a_to, const RayFn& a_ray, int& a_rays)
		{
			const Vec3  d = a_to - a_from;
			const float len = Length(d);
			if (!(len > 1e-3f)) {
				return true;
			}
			++a_rays;
			const auto h = a_ray(a_from, d, Ray::kWorld);
			return !h || Length(h->pos - a_from) >= len - 0.3f;
		}

		// The floor, footprint, headroom and knee rules (the fallback's, and the start of every candidate's): fills a_s.feet.y.
		Why Ground(Spot& a_s, const Scene& a_scene, const RayFn& a_ray, const Rules& a_r, int& a_rays, float* a_dh)
		{
			const float top = a_scene.floorY + a_r.floorAbove;
			++a_rays;
			const auto floor = a_ray(Vec3{ a_s.feet.x, top, a_s.feet.z }, Vec3{ 0, -(a_r.floorAbove + a_r.floorBelow), 0 }, Ray::kGround);
			if (!floor) {
				return Why::kNoFloor;
			}
			const float h = floor->pos.y;
			a_s.feet.y = h;
			*a_dh = h - a_scene.floorY;
			if (std::fabs(*a_dh) > a_r.step) {
				return Why::kStep;
			}
			if (std::isfinite(floor->normal.y) && floor->normal.y < a_r.slopeNy) {
				return Why::kSlope;
			}
			float lo = h, hi = h;
			for (const auto& o : { Vec3{ a_r.footprint, 0, 0 }, Vec3{ -a_r.footprint, 0, 0 }, Vec3{ 0, 0, a_r.footprint }, Vec3{ 0, 0, -a_r.footprint } }) {
				++a_rays;
				const auto f = a_ray(Vec3{ a_s.feet.x + o.x, h + 1.0f, a_s.feet.z + o.z }, Vec3{ 0, -2.5f, 0 }, Ray::kGround);
				if (!f) {
					return Why::kUneven;
				}
				lo = std::min(lo, f->pos.y);
				hi = std::max(hi, f->pos.y);
			}
			if (hi - lo > a_r.spread) {
				return Why::kUneven;
			}
			++a_rays;
			if (a_ray(Vec3{ a_s.feet.x, h + 0.05f, a_s.feet.z }, Vec3{ 0, a_r.headroom, 0 }, Ray::kWorld)) {
				return Why::kHeadroom;
			}
			for (const float kh : { a_r.kneeHigh, a_r.kneeLow }) {
				for (int j = 0; j < 8; ++j) {
					const float b = kTwoPi * static_cast<float>(j) / 8.0f;
					++a_rays;
					if (a_ray(Vec3{ a_s.feet.x, h + kh, a_s.feet.z }, Vec3{ a_r.kneeOut * std::cos(b), 0, a_r.kneeOut * std::sin(b) }, Ray::kWorld)) {
						return Why::kKnee;
					}
				}
			}
			return Why::kOk;
		}
	}

	std::vector<Spot> Candidates(const Scene& a_s, const Rules& a_r)
	{
		std::vector<Spot> out;
		for (const float r : a_r.rings) {
			for (int k = 0; k < a_r.dirs; ++k) {
				const float a = kTwoPi * static_cast<float>(k) / static_cast<float>(a_r.dirs);
				Spot        s;
				s.r = r;
				s.deg = a * 180.0f / std::numbers::pi_v<float>;
				s.feet = Vec3{ a_s.centre.x + r * std::cos(a), a_s.floorY, a_s.centre.z + r * std::sin(a) };
				s.heading = Heading(a_s.centre - s.feet);
				bool front = true;
				s.screenX = ScreenXOf(a_s, s.feet, a_r, &front);
				s.expected = Score(r, s.screenX, 0.0f, a_r) + (front ? 0.0f : 10.0f);
				out.push_back(s);
			}
		}
		std::stable_sort(out.begin(), out.end(), [](const Spot& a, const Spot& b) { return a.expected < b.expected; });
		return out;
	}

	Spot Evaluate(Spot a_s, const Scene& a_scene, const RayFn& a_ray, const Rules& a_r, int* a_rays)
	{
		int  rays = 0;
		auto done = [&](Why a_why) {
			a_s.why = a_why;
			if (a_rays) {
				*a_rays += rays;
			}
			return a_s;
		};
		// The rules without rays first.
		if (LengthXZ(a_s.feet - a_scene.snake) < a_r.snakeClear) {
			return done(Why::kOnSnake);
		}
		for (const auto& o : a_scene.others) {
			if (LengthXZ(a_s.feet - o) < a_r.charClear && std::fabs(o.y - a_s.feet.y) < 3.0f) {
				return done(Why::kCharacter);
			}
		}
		const auto viewRules = [&](float a_y) -> Why {
			if (!a_scene.view.valid) {
				return Why::kOk;
			}
			const auto p = Project(a_scene.view, Vec3{ a_s.feet.x, a_y + a_r.bodyHeight, a_s.feet.z });
			if (!p.front) {
				return Why::kBehindView;
			}
			if (p.x < 0.0f || p.x > 1.0f || p.y < 0.0f || p.y > 1.0f) {
				return Why::kOffScreen;
			}
			a_s.screenX = p.x;
			if (p.x < a_r.menuX) {
				return Why::kUnderMenu;
			}
			const auto sp = Project(a_scene.view, a_scene.snake + Vec3{ 0, a_r.snakeHeight, 0 });
			if (sp.front) {
				const float aspect = a_scene.view.aspect > 0.1f ? a_scene.view.aspect : 16.0f / 9.0f;
				const float half = a_r.snakeRadius / (sp.depth * std::tan(a_scene.view.fovY * 0.5f) * aspect) * 0.5f;
				if (std::fabs(p.x - sp.x) < half + 0.02f) {
					return sp.depth < p.depth ? Why::kBehindSnake : Why::kCoversSnake;
				}
			}
			// The renderer's occluder fade: any part of her within occluderClear of the camera -> Snake's chest line, in front of him.
			const Vec3  chest = a_scene.snake + Vec3{ 0, 1.0f, 0 };
			const Vec3  line = chest - a_scene.view.pos;
			const float toSnake = Length(line);
			if (toSnake > 1e-3f) {
				const Vec3 dir = line * (1.0f / toSnake);
				for (const float h : { 0.3f, 0.7f, 1.1f }) {
					const Vec3  q = Vec3{ a_s.feet.x, a_y + h, a_s.feet.z } - a_scene.view.pos;
					const float along = Dot(q, dir);
					if (along > 0.2f && along < toSnake - 0.3f && Length(q - dir * along) < a_r.occluderClear) {
						return Why::kCoversSnake;
					}
				}
			}
			return Why::kOk;
		};
		if (const auto w = viewRules(a_scene.floorY); w != Why::kOk) {
			return done(w);
		}
		float dh = 0;
		if (const auto w = Ground(a_s, a_scene, a_ray, a_r, rays, &dh); w != Why::kOk) {
			return done(w);
		}
		if (const auto w = viewRules(a_s.feet.y); w != Why::kOk) {  // again on the floor found (dh <= seat_step)
			return done(w);
		}
		if (!LineClear(Vec3{ a_scene.centre.x, a_scene.floorY + a_r.reachHeight, a_scene.centre.z }, a_s.feet + Vec3{ 0, a_r.reachHeight, 0 }, a_ray, rays)) {
			return done(Why::kReach);
		}
		if (a_scene.view.valid && !LineClear(a_scene.view.pos, a_s.feet + Vec3{ 0, a_r.bodyHeight, 0 }, a_ray, rays)) {
			return done(Why::kViewBlocked);
		}
		a_s.score = Score(a_s.r, a_s.screenX, dh, a_r);
		a_s.heading = Heading(a_scene.centre - a_s.feet);
		return done(Why::kOk);
	}

	Spot Fallback(const Scene& a_scene, const RayFn& a_ray, const Rules& a_r, int* a_rays)
	{
		Spot s;
		s.fallback = true;
		s.r = a_r.fallbackR;
		Vec3 away{ a_scene.centre.x - a_scene.snake.x, 0, a_scene.centre.z - a_scene.snake.z };
		away = LengthXZ(away) > 1e-3f ? Normalize(away) : Vec3{ 1, 0, 0 };
		s.feet = Vec3{ a_scene.centre.x + away.x * s.r, a_scene.floorY, a_scene.centre.z + away.z * s.r };
		s.deg = std::atan2(away.z, away.x) * 180.0f / std::numbers::pi_v<float>;
		int   rays = 0;
		float dh = 0;
		s.why = Ground(s, a_scene, a_ray, a_r, rays, &dh);
		// The character and reach rules as for any candidate (review: a seat behind a thin wall or balustrade, or beside an NPC).
		if (s.why == Why::kOk) {
			for (const auto& o : a_scene.others) {
				if (LengthXZ(s.feet - o) < a_r.charClear && std::fabs(o.y - s.feet.y) < 3.0f) {
					s.why = Why::kCharacter;
					break;
				}
			}
		}
		if (s.why == Why::kOk &&
			!LineClear(Vec3{ a_scene.centre.x, a_scene.floorY + a_r.reachHeight, a_scene.centre.z }, s.feet + Vec3{ 0, a_r.reachHeight, 0 }, a_ray, rays)) {
			s.why = Why::kReach;
		}
		s.heading = Heading(a_scene.centre - s.feet);
		s.screenX = ScreenXOf(a_scene, s.feet, a_r);
		s.score = Score(s.r, s.screenX, dh, a_r);
		if (a_rays) {
			*a_rays += rays;
		}
		return s;
	}

	void Search::Start(Scene a_scene, const Rules& a_rules)
	{
		*this = {};
		scene_ = std::move(a_scene);
		rules_ = a_rules;
		order_ = Candidates(scene_, rules_);
		bestPossible_ = order_.empty() ? 0.0f : order_.front().expected;
		running_ = true;
	}

	bool Search::Step(int a_rayBudget, const RayFn& a_ray)
	{
		if (!running_) {
			return done_;
		}
		const int before = rays_;
		while (next_ < order_.size() && (rays_ == before || rays_ - before < a_rayBudget)) {
			const auto s = Evaluate(order_[next_++], scene_, a_ray, rules_, &rays_);
			++evaluated_;
			++counts_[static_cast<std::size_t>(s.why)];
			if (s.why != Why::kOk) {
				continue;
			}
			++passed_;
			if (!best_ || s.score < best_->score) {
				best_ = s;
			}
			if (s.score <= bestPossible_ + rules_.goodEnough) {
				next_ = order_.size();  // good enough: stop here
			}
		}
		if (next_ < order_.size()) {
			return false;
		}
		if (best_) {
			result_ = best_;
		} else {
			const auto f = Fallback(scene_, a_ray, rules_, &rays_);
			++counts_[static_cast<std::size_t>(f.why)];
			if (f.why == Why::kOk) {
				result_ = f;
			}
		}
		running_ = false;
		done_ = true;
		return true;
	}

	void Search::Shift(const Vec3& a_d)
	{
		scene_.centre += a_d;
		scene_.floorY += a_d.y;
		scene_.snake += a_d;
		scene_.view.pos += a_d;
		for (auto& o : scene_.others) {
			o += a_d;
		}
		for (auto& s : order_) {
			s.feet += a_d;
		}
		if (best_) {
			best_->feet += a_d;
		}
		if (result_) {
			result_->feet += a_d;
		}
	}

	std::string Search::Summary() const
	{
		std::string why;
		for (std::size_t i = 1; i < counts_.size(); ++i) {
			if (counts_[i]) {
				why += std::format("{}{} {}", why.empty() ? "" : ", ", Name(static_cast<Why>(i)), counts_[i]);
			}
		}
		return std::format("{} of {} candidates looked at, {} passed, {} rays{}", evaluated_, order_.size(), passed_, rays_,
			why.empty() ? std::string() : std::format(" (refused: {})", why));
	}
}
