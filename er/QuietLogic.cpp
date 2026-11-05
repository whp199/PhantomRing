#include "QuietLogic.h"

#include "Config.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace phantom::er::quiet
{
	namespace
	{
		constexpr float kPi = std::numbers::pi_v<float>;
		constexpr float kTwoPi = 2.0f * kPi;

		std::vector<float> Rings(const std::vector<float>& a_in, float a_min, float a_max)
		{
			std::vector<float> out;
			for (const float r : a_in) {
				if (std::isfinite(r) && r >= a_min && r <= a_max) {
					out.push_back(r);
				}
			}
			return out;
		}

		float Pos(float a_v) { return std::isfinite(a_v) ? std::max(0.0f, a_v) : 0.0f; }
	}

	Rules MakeRules(const QuietConfig& a_c)
	{
		Rules r;
		r.coverRings = Rings(a_c.coverRings, 3.0f, 120.0f);
		r.nearRings = Rings(a_c.nearRings, 2.0f, 60.0f);
		if (r.coverRings.empty()) {
			r.coverRings = { 35.0f };
		}
		if (r.nearRings.empty()) {
			r.nearRings = { 12.0f };
		}
		r.dirs = std::clamp(a_c.postDirs, 4, 72);
		r.coverHeightMin = a_c.postHeightMin;
		r.coverHeightMax = a_c.postHeightMax;
		r.nearHeightMin = a_c.nearHeightMin;
		r.nearHeightMax = a_c.nearHeightMax;
		r.drawable = a_c.postDrawable;
		r.heightScore = a_c.postHeightScore;
		r.sight = Pos(a_c.sightHeight);
		r.max3d = Pos(a_c.postMax3d);
		r.enemyClear = Pos(a_c.postEnemyClear);
		r.charClear = Pos(a_c.postCharClear);
		r.raysPerFrame = std::clamp(a_c.postRaysPerFrame, 4, 400);
		r.researchCover = Pos(a_c.postResearchSnake);
		r.researchNear = Pos(a_c.nearResearchSnake);
		r.moveSpeed = Pos(a_c.moveHideSpeed);
		r.fadeMs = std::max(0, a_c.fadeMs);
		r.farUp = Pos(a_c.farGroundUp);
		r.farDown = Pos(a_c.farGroundDown);
		r.liftMax = Pos(a_c.liftMax);
		r.targetRange = Pos(a_c.targetRange);
		r.coneDeg = Pos(a_c.targetConeDeg);
		r.coneBinocularsDeg = Pos(a_c.targetConeBinocularsDeg);
		r.markedFirst = a_c.targetMarkedFirst;
		r.latchMs = std::max(0, a_c.pickLatchMs);
		r.spotRange = Pos(a_c.spotRange);
		r.spotEveryMs = std::max(50, a_c.spotEveryMs);
		r.spotLinger = Pos(a_c.spotLinger);
		r.spotTextEvery = Pos(a_c.spotTextEvery);
		r.lethalShareMult = Pos(a_c.lethalShareMult);
		r.lethalFallback = Pos(a_c.lethalFallback);
		r.heavyPoise = a_c.heavyPoise;
		r.tranqSleep = Pos(a_c.tranqSleep);
		r.coverThreatRange = Pos(a_c.coverThreatRange);
		return r;
	}

	bool LineClear(const Vec3& a_from, const Vec3& a_to, const RayFn& a_ray, float a_slack, int* a_rays)
	{
		const Vec3  d = a_to - a_from;
		const float len = Length(d);
		if (!(len > 1e-3f)) {
			return true;
		}
		if (a_rays) {
			++*a_rays;
		}
		const auto h = a_ray(a_from, d, Ray::kWorld);
		return !h || Length(h->pos - a_from) >= len - a_slack;
	}

	const char* Name(Mode a_m)
	{
		switch (a_m) {
		case Mode::kNone: return "none";
		case Mode::kCover: return "cover";
		case Mode::kNear: return "near";
		case Mode::kMoving: return "on the move";
		}
		return "?";
	}

	const char* Name(PostWhy a_w)
	{
		switch (a_w) {
		case PostWhy::kOk: return "ok";
		case PostWhy::kNoGround: return "no ground";
		case PostWhy::kSlope: return "too steep";
		case PostWhy::kTooLow: return "too low";
		case PostWhy::kTooHigh: return "too high";
		case PostWhy::kTooFar: return "too far (3D)";
		case PostWhy::kNotDrawable: return "not drawable (outside MGSV's view of her body)";
		case PostWhy::kEnemyNear: return "a hostile within 15 m";
		case PostWhy::kCharNear: return "a character within 3 m";
		case PostWhy::kFootprint: return "a ledge (footprint)";
		case PostWhy::kHeadroom: return "no headroom";
		case PostWhy::kNoSnake: return "no line to Snake";
		case PostWhy::kNoEnemy: return "sees no hostile";
		case PostWhy::kNoAim: return "no line to the view's aim";
		case PostWhy::kCount: break;
		}
		return "?";
	}

	float DrawableBound(float a_h, float a_fovY)
	{
		const float fov = std::isfinite(a_fovY) && a_fovY > 0.05f ? std::min(a_fovY, 2.5f) : 0.8f;
		return a_h * std::tan(fov * 0.5f) - 2.0f;
	}

	float PostScore(Mode a_mode, int a_seen, float a_h, float a_height, float a_fovY, const Rules& a_r)
	{
		const float bound = std::max(0.0f, DrawableBound(a_h, a_fovY));
		const float prefer = a_mode == Mode::kCover ? 35.0f : 12.0f;
		return 3.0f * static_cast<float>(a_seen) - 0.05f * std::fabs(a_h - prefer) + a_r.heightScore * std::min(a_height, a_r.drawable ? bound : a_height);
	}

	std::vector<Post> PostCandidates(const PostScene& a_s, Mode a_mode, const Rules& a_r)
	{
		std::vector<Post> out;
		const auto&       rings = a_mode == Mode::kCover ? a_r.coverRings : a_r.nearRings;
		const float       prefer = a_mode == Mode::kCover ? 35.0f : 12.0f;
		Vec3              f{ a_s.camFwd.x, 0, a_s.camFwd.z };
		f = LengthXZ(f) > 1e-4f ? Normalize(f) : Vec3{ 0, 0, 1 };
		struct Keyed
		{
			Post  p;
			float key;
		};
		std::vector<Keyed> k;
		for (const float r : rings) {
			for (int i = 0; i < a_r.dirs; ++i) {
				const float a = kTwoPi * static_cast<float>(i) / static_cast<float>(a_r.dirs);
				Post        p;
				p.mode = a_mode;
				p.ring = r;
				p.deg = a * 180.0f / kPi;
				p.feet = Vec3{ a_s.snake.x + r * std::cos(a), a_s.snake.y, a_s.snake.z + r * std::sin(a) };
				const float c = Dot(Vec3{ std::cos(a), 0, std::sin(a) }, f);  // 1 straight ahead, -1 behind
				p.forward = c >= 0.0f;
				k.push_back({ p, (p.forward ? 0.0f : 1000.0f) + std::fabs(r - prefer) * 2.0f + (1.0f - c) });
			}
		}
		std::stable_sort(k.begin(), k.end(), [](const Keyed& a, const Keyed& b) { return a.key < b.key; });
		out.reserve(k.size());
		for (auto& x : k) {
			out.push_back(x.p);
		}
		return out;
	}

	Post EvaluatePost(Post a_p, const PostScene& a_s, const RayFn& a_ray, const Rules& a_r, int* a_rays)
	{
		int  rays = 0;
		auto done = [&](PostWhy a_w) {
			a_p.why = a_w;
			if (a_rays) {
				*a_rays += rays;
			}
			return a_p;
		};
		const bool cover = a_p.mode == Mode::kCover;
		const float top = a_s.snake.y + a_r.groundAbove;
		++rays;
		const auto g = a_ray(Vec3{ a_p.feet.x, top, a_p.feet.z }, Vec3{ 0, -2.0f * a_r.groundAbove, 0 }, Ray::kGround);
		if (!g) {
			return done(PostWhy::kNoGround);
		}
		a_p.feet.y = g->pos.y;
		if (std::isfinite(g->normal.y) && g->normal.y < a_r.slopeNy) {
			return done(PostWhy::kSlope);
		}
		a_p.height = a_p.feet.y - a_s.snake.y;
		if (a_p.height < (cover ? a_r.coverHeightMin : a_r.nearHeightMin)) {
			return done(PostWhy::kTooLow);
		}
		if (a_p.height > (cover ? a_r.coverHeightMax : a_r.nearHeightMax)) {
			return done(PostWhy::kTooHigh);
		}
		if (Length(a_p.feet - a_s.snake) > a_r.max3d) {
			return done(PostWhy::kTooFar);
		}
		const float horizontal = LengthXZ(a_p.feet - a_s.snake);
		if (a_r.drawable && std::fabs(a_p.height) > DrawableBound(horizontal, a_s.fovY)) {
			return done(PostWhy::kNotDrawable);
		}
		for (const auto& h : a_s.hostiles) {
			if (Length(h.feet - a_p.feet) < a_r.enemyClear) {
				return done(PostWhy::kEnemyNear);
			}
		}
		for (const auto& c : a_s.characters) {
			if (Length(c - a_p.feet) < a_r.charClear) {
				return done(PostWhy::kCharNear);
			}
		}
		for (const auto& o : { Vec3{ a_r.footprint, 0, 0 }, Vec3{ -a_r.footprint, 0, 0 }, Vec3{ 0, 0, a_r.footprint }, Vec3{ 0, 0, -a_r.footprint } }) {
			++rays;
			const auto f = a_ray(Vec3{ a_p.feet.x + o.x, a_p.feet.y + 2.0f, a_p.feet.z + o.z }, Vec3{ 0, -4.0f, 0 }, Ray::kGround);
			if (!f || std::fabs(f->pos.y - a_p.feet.y) > a_r.footTol) {
				return done(PostWhy::kFootprint);
			}
		}
		++rays;
		if (a_ray(Vec3{ a_p.feet.x, a_p.feet.y + 0.1f, a_p.feet.z }, Vec3{ 0, a_r.headroom, 0 }, Ray::kWorld)) {
			return done(PostWhy::kHeadroom);
		}
		const Vec3 eye = a_p.feet + Vec3{ 0, a_r.sight, 0 };
		if (!LineClear(eye, a_s.snakeChest, a_ray, a_r.lineSlack, &rays)) {
			return done(PostWhy::kNoSnake);
		}
		Vec3 face{ a_s.camFwd.x, 0, a_s.camFwd.z };  // Near: along the view
		if (cover) {
			// The nearest hostiles to Snake first, at most maxSeen of them.
			std::vector<const Foe*> foes;
			for (const auto& h : a_s.hostiles) {
				foes.push_back(&h);
			}
			std::ranges::sort(foes, [&](const Foe* a, const Foe* b) { return LengthXZ(a->feet - a_s.snake) < LengthXZ(b->feet - a_s.snake); });
			if (foes.size() > static_cast<std::size_t>(std::max(1, a_r.maxSeen))) {
				foes.resize(static_cast<std::size_t>(std::max(1, a_r.maxSeen)));
			}
			Vec3 sum{};
			for (const auto* h : foes) {
				if (LineClear(eye, h->chest, a_ray, a_r.lineSlack, &rays)) {
					++a_p.seen;
					sum += Vec3{ h->feet.x - a_p.feet.x, 0, h->feet.z - a_p.feet.z };
				}
			}
			if (a_p.seen == 0) {
				return done(PostWhy::kNoEnemy);
			}
			face = sum;
		} else if (Finite(a_s.aimPoint) && !LineClear(eye, a_s.aimPoint, a_ray, a_r.lineSlack + 1.0f, &rays)) {
			return done(PostWhy::kNoAim);
		}
		a_p.heading = LengthXZ(face) > 1e-3f ? Heading(face) : Heading(a_s.snake - a_p.feet);
		a_p.score = PostScore(a_p.mode, a_p.seen, horizontal, a_p.height, a_s.fovY, a_r);
		return done(PostWhy::kOk);
	}

	bool Better(const Post& a_c, const Post& a_cur, const Rules& a_r)
	{
		if (a_c.mode != a_cur.mode) {
			return true;
		}
		return a_c.score - a_cur.score > a_r.better * std::max(1.0f, std::fabs(a_cur.score));
	}

	void PostSearch::Start(PostScene a_scene, Mode a_mode, const Rules& a_rules)
	{
		*this = {};
		scene_ = std::move(a_scene);
		rules_ = a_rules;
		mode_ = a_mode;
		order_ = PostCandidates(scene_, mode_, rules_);
		running_ = true;
	}

	bool PostSearch::Step(int a_budget, const RayFn& a_ray)
	{
		if (!running_) {
			return done_;
		}
		const int before = rays_;
		while (next_ < order_.size() && (rays_ == before || rays_ - before < a_budget)) {
			// The forward half is done and something passed there: enough.
			if (!order_[next_].forward && best_ && best_->forward) {
				next_ = order_.size();
				break;
			}
			const auto p = EvaluatePost(order_[next_++], scene_, a_ray, rules_, &rays_);
			++evaluated_;
			++counts_[static_cast<std::size_t>(p.why)];
			if (p.why == PostWhy::kOk) {
				++passed_;
				if (!best_ || p.score > best_->score) {
					best_ = p;
				}
			}
		}
		if (next_ < order_.size()) {
			return false;
		}
		running_ = false;
		done_ = true;
		return true;
	}

	void PostSearch::Shift(const Vec3& a_d)
	{
		scene_.snake += a_d;
		scene_.snakeChest += a_d;
		scene_.camPos += a_d;
		scene_.aimPoint += a_d;
		for (auto& h : scene_.hostiles) {
			h.feet += a_d;
			h.chest += a_d;
		}
		for (auto& c : scene_.characters) {
			c += a_d;
		}
		for (auto& p : order_) {
			p.feet += a_d;
		}
		if (best_) {
			best_->feet += a_d;
		}
	}

	std::string PostSearch::Summary() const
	{
		std::string why;
		for (std::size_t i = 1; i < counts_.size(); ++i) {
			if (counts_[i]) {
				why += std::format("{}{} {}", why.empty() ? "" : ", ", Name(static_cast<PostWhy>(i)), counts_[i]);
			}
		}
		return std::format("{} {} of {} candidates looked at, {} passed, {} rays{}", Name(mode_), evaluated_, order_.size(), passed_, rays_,
			why.empty() ? std::string() : std::format(" (refused: {})", why));
	}

	Mode MoveWatch::Update(float a_speed, float a_dt, bool a_hostileNear, const Rules& a_r)
	{
		const float dt = std::isfinite(a_dt) && a_dt > 0.0f ? std::min(a_dt, 0.25f) : 0.0f;
		const float v = std::isfinite(a_speed) ? a_speed : 0.0f;
		fast_ = v > a_r.moveSpeed ? fast_ + dt : 0.0f;
		slow_ = v < a_r.stopSpeed ? slow_ + dt : 0.0f;
		if (!moving_ && fast_ >= a_r.moveSeconds && !a_hostileNear) {
			moving_ = true;
		} else if (moving_ && (slow_ >= a_r.stopSeconds || a_hostileNear)) {
			moving_ = false;
		}
		if (moving_) {
			return Mode::kMoving;
		}
		return a_hostileNear ? Mode::kCover : Mode::kNear;
	}

	const char* NeedSearch(const Need& a_n, const Rules& a_r)
	{
		if (a_n.mode == Mode::kMoving || a_n.mode == Mode::kNone) {
			return nullptr;
		}
		if (!a_n.havePost) {
			return "she has no post";
		}
		if (a_n.postMode != a_n.mode) {
			return a_n.mode == Mode::kCover ? "hostiles near (cover)" : "calm again (near)";
		}
		if (a_n.snakeFromCentre > (a_n.mode == Mode::kCover ? a_r.researchCover : a_r.researchNear)) {
			return "Snake moved on";
		}
		if (a_n.lostSnake) {
			return "her post lost its line to Snake";
		}
		if (a_n.threatNear) {
			return "a hostile came near her post";
		}
		if (a_n.targetUnseen) {
			return "the enemy in view can't be seen from her post";
		}
		return nullptr;
	}

	void Fader::Want(std::optional<Post> a_post, bool a_inView, std::uint64_t a_nowMs)
	{
		pending_ = std::move(a_post);
		havePending_ = true;
		echoLate_ = false;
		if (a_inView && shown_ && alpha_ > 0.0f) {
			if (step_ != Step::kOut) {
				// Out from where she is (a fade-in cut short fades out from its alpha).
				step_ = Step::kOut;
				since_ = a_nowMs ? a_nowMs : 1;
				from_ = alpha_;
			}
			return;
		}
		step_ = Step::kOut;
		since_ = 0;  // at once
		from_ = 0.0f;
	}

	bool Fader::Update(std::uint64_t a_nowMs, bool a_echo, bool a_inView, const Rules& a_r)
	{
		const float len = static_cast<float>(std::max(1, a_r.fadeMs));
		switch (step_) {
		case Step::kSteady:
			alpha_ = shown_ ? 1.0f : 0.0f;
			return false;
		case Step::kOut: {
			const float t = since_ ? static_cast<float>(a_nowMs - since_) / len : 1.0f;
			alpha_ = shown_ ? std::clamp(from_ * (1.0f - t), 0.0f, 1.0f) : 0.0f;
			if (t < 1.0f && alpha_ > 0.0f) {
				return false;
			}
			alpha_ = 0.0f;
			shown_ = pending_;
			pending_.reset();
			havePending_ = false;
			if (!shown_) {
				step_ = Step::kSteady;
				return true;
			}
			step_ = Step::kWaitEcho;
			since_ = a_nowMs ? a_nowMs : 1;
			return true;
		}
		case Step::kWaitEcho:
			alpha_ = 0.0f;
			if (a_echo || a_nowMs - since_ >= static_cast<std::uint64_t>(std::max(0, a_r.echoWaitMs))) {
				echoLate_ = !a_echo;
				if (a_inView) {
					step_ = Step::kIn;
					since_ = a_nowMs ? a_nowMs : 1;
				} else {
					step_ = Step::kSteady;
					alpha_ = 1.0f;
				}
			}
			return false;
		case Step::kIn: {
			const float t = static_cast<float>(a_nowMs - since_) / len;
			alpha_ = std::clamp(t, 0.0f, 1.0f);
			if (t >= 1.0f) {
				step_ = Step::kSteady;
				alpha_ = 1.0f;
			}
			return false;
		}
		}
		return false;
	}

	void Fader::Shift(const Vec3& a_d)
	{
		if (shown_) {
			shown_->feet += a_d;
		}
		if (pending_) {
			pending_->feet += a_d;
		}
	}

	std::optional<float> FarGround(std::span<const Hit> a_hits)
	{
		for (const auto& h : a_hits) {
			if (!std::isfinite(h.pos.y)) {
				continue;
			}
			if (!std::isfinite(h.normal.y) || h.normal.y >= 0.7f) {
				return h.pos.y;
			}
		}
		return std::nullopt;
	}

	// ---- her target --------------------------------------------------------------------------------------------------------
	const char* Name(PickWhy a_w)
	{
		switch (a_w) {
		case PickWhy::kOk: return "ok";
		case PickWhy::kNotHostile: return "not hostile (never a candidate)";
		case PickWhy::kDead: return "dead";
		case PickWhy::kHidden: return "not rendered";
		case PickWhy::kBoss: return "a boss (never)";
		case PickWhy::kHeld: return "held by CQC";
		case PickWhy::kLifting: return "on a Fulton balloon";
		case PickWhy::kAsleep: return "asleep (the Guilty Butterfly)";
		case PickWhy::kNoPost: return "Quiet has no post";
		case PickWhy::kTooFar: return "too far from her post";
		case PickWhy::kOutside: return "outside the Actors radius";
		case PickWhy::kOffCentre: return "off the view's centre";
		case PickWhy::kNoSightPost: return "not seen from her post";
		case PickWhy::kNoSightView: return "not seen from the camera";
		case PickWhy::kNobody: return "nobody";
		case PickWhy::kCount: break;
		}
		return "?";
	}

	namespace
	{
		PickWhy Hard(const Target& a_t, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius, bool a_tranq, const Rules& a_r)
		{
			if (!a_t.hostile) {
				return PickWhy::kNotHostile;  // first, whatever else he is
			}
			if (!a_t.alive) {
				return PickWhy::kDead;
			}
			if (!a_t.rendered) {
				return PickWhy::kHidden;
			}
			if (a_t.boss) {
				return PickWhy::kBoss;
			}
			if (a_t.held) {
				return PickWhy::kHeld;
			}
			if (a_t.lifting) {
				return PickWhy::kLifting;
			}
			if (a_tranq && a_t.asleep) {
				return PickWhy::kAsleep;
			}
			if (!a_sight) {
				return PickWhy::kNoPost;
			}
			if (Length(a_t.feet - a_snake) > a_actorRadius) {
				return PickWhy::kOutside;
			}
			if (Length(a_t.chest - *a_sight) > a_r.targetRange) {
				return PickWhy::kTooFar;
			}
			return PickWhy::kOk;
		}

		float OffAxis(const Target& a_t, const View& a_v)
		{
			const Vec3  d = a_t.chest - a_v.pos;
			const float len = Length(d);
			const Vec3  f = Normalize(a_v.fwd);
			if (!(len > 1e-3f)) {
				return 0.0f;
			}
			const float c = std::clamp(Dot(d, f) / len, -1.0f, 1.0f);
			return std::max(0.0f, std::acos(c) - std::atan(std::max(0.0f, a_t.radius) / len));
		}
	}

	PickWhy Screen(const Target& a_t, const View& a_v, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius, bool a_tranq, bool a_bino,
		const Rules& a_r, float* a_off)
	{
		const float off = OffAxis(a_t, a_v);
		if (a_off) {
			*a_off = off;
		}
		if (const auto w = Hard(a_t, a_sight, a_snake, a_actorRadius, a_tranq, a_r); w != PickWhy::kOk) {
			return w;
		}
		const float cone = (a_bino ? a_r.coneBinocularsDeg : a_r.coneDeg) * kPi / 180.0f;
		return off <= cone ? PickWhy::kOk : PickWhy::kOffCentre;
	}

	bool LatchFits(const Target& a_t, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius, bool a_tranq, const Rules& a_r)
	{
		return Hard(a_t, a_sight, a_snake, a_actorRadius, a_tranq, a_r) == PickWhy::kOk;
	}

	PickResult Pick(std::span<const Target> a_ts, const View& a_v, const std::optional<Vec3>& a_sight, const Vec3& a_snake, float a_actorRadius, bool a_tranq,
		bool a_bino, const Rules& a_r, const SightFn& a_fromPost, const SightFn& a_fromView, std::size_t a_maxChecks)
	{
		PickResult res;
		struct Cand
		{
			std::size_t i;
			float       off;
			bool        marked;
		};
		std::vector<Cand> ok;
		for (std::size_t i = 0; i < a_ts.size(); ++i) {
			float      off = kPi;
			const auto w = Screen(a_ts[i], a_v, a_sight, a_snake, a_actorRadius, a_tranq, a_bino, a_r, &off);
			if (w == PickWhy::kOk) {
				ok.push_back({ i, off, a_ts[i].marked });
				continue;
			}
			// The best-placed hostile one that wasn't taken, and why (the corner, the dry run).
			if (a_ts[i].hostile && off < res.nearestOff) {
				res.nearestOff = off;
				res.nearest = i;
				res.why = w;
			}
		}
		std::ranges::sort(ok, [&](const Cand& a, const Cand& b) {
			if (a_r.markedFirst && a.marked != b.marked) {
				return a.marked;
			}
			return a.off < b.off;
		});
		for (const auto& c : ok) {
			if (res.checked >= a_maxChecks) {
				break;
			}
			++res.checked;
			const auto& t = a_ts[c.i];
			if (a_fromPost && !a_fromPost(t)) {
				if (c.off < res.nearestOff || res.why != PickWhy::kNoSightPost) {
					res.nearest = c.i;
					res.nearestOff = c.off;
					res.why = PickWhy::kNoSightPost;
				}
				continue;
			}
			if (a_fromView && !a_fromView(t)) {
				if (c.off < res.nearestOff || res.why != PickWhy::kNoSightView) {
					res.nearest = c.i;
					res.nearestOff = c.off;
					res.why = PickWhy::kNoSightView;
				}
				continue;
			}
			res.index = c.i;
			res.why = PickWhy::kOk;
			return res;
		}
		if (a_ts.empty()) {
			res.why = PickWhy::kNobody;
		}
		return res;
	}

	// ---- spotting ----------------------------------------------------------------------------------------------------------
	std::vector<std::uint64_t> Spotter::Update(std::span<const std::uint64_t> a_seen, std::uint64_t a_nowMs, const Rules& a_r)
	{
		std::vector<std::uint64_t> fresh;
		const auto                 until = a_nowMs + static_cast<std::uint64_t>(std::max(0.0f, a_r.spotLinger) * 1000.0f);
		for (const auto h : a_seen) {
			auto it = std::ranges::find_if(until_, [&](const auto& e) { return e.first == h; });
			if (it == until_.end()) {
				until_.emplace_back(h, until);
				fresh.push_back(h);
			} else {
				if (it->second <= a_nowMs) {
					fresh.push_back(h);  // spotted again after his mark lapsed
				}
				it->second = until;
			}
		}
		std::erase_if(until_, [&](const auto& e) { return e.second + 60000 < a_nowMs; });
		return fresh;
	}

	void Spotter::Forget(std::uint64_t a_handle)
	{
		std::erase_if(until_, [&](const auto& e) { return e.first == a_handle; });
	}

	std::uint64_t Spotter::Until(std::uint64_t a_handle) const
	{
		for (const auto& [h, u] : until_) {
			if (h == a_handle) {
				return u;
			}
		}
		return 0;
	}

	std::size_t Spotter::Count(std::uint64_t a_nowMs) const
	{
		return static_cast<std::size_t>(std::ranges::count_if(until_, [&](const auto& e) { return e.second > a_nowMs; }));
	}

	std::string SpotText::Update(std::size_t a_fresh, std::size_t a_total, std::uint64_t a_nowMs, float a_everyS)
	{
		const auto every = static_cast<std::uint64_t>(std::max(0.0f, a_everyS) * 1000.0f);
		if (a_fresh) {
			pending_ = true;
		}
		if (!pending_ || a_total == 0) {
			pending_ = pending_ && a_total > 0;
			return {};
		}
		if (said_ && a_nowMs - last_ < every) {
			return {};
		}
		said_ = true;
		pending_ = false;
		last_ = a_nowMs;
		return a_total > 1 ? std::format("Quiet: Enemy spotted ({} nearby)", a_total) : std::string("Quiet: Enemy spotted");
	}

	// ---- her shot ----------------------------------------------------------------------------------------------------------
	proto::QuietWeapon WeaponOfAttack(std::uint32_t a_id)
	{
		switch (a_id) {
		case 290: return proto::kQuietLethal;
		case 291: return proto::kQuietTranq;
		case 292: return proto::kQuietHeavy;
		default: return proto::kQuietWeaponUnknown;
		}
	}

	const char* WeaponName(proto::QuietWeapon a_w)
	{
		switch (a_w) {
		case proto::kQuietLethal: return "Wicked Butterfly (lethal)";
		case proto::kQuietTranq: return "Guilty Butterfly (tranquilizer)";
		case proto::kQuietHeavy: return "Sinful Butterfly (anti-materiel)";
		default: return "unknown rifle";
		}
	}

	const char* Name(ShotAct a_a)
	{
		switch (a_a) {
		case ShotAct::kNone: return "nothing";
		case ShotAct::kLethal: return "lethal";
		case ShotAct::kHeavy: return "heavy (lethal + knockdown)";
		case ShotAct::kSleep: return "sleep";
		case ShotAct::kNotHostile: return "refused: NOT a hostile enemy";
		case ShotAct::kBoss: return "refused: a boss";
		case ShotAct::kDead: return "refused: dead already";
		case ShotAct::kGone: return "refused: gone";
		case ShotAct::kNoPost: return "dropped: no post";
		case ShotAct::kBlocked: return "missed: no clear line from her post";
		case ShotAct::kUnknownRifle: return "refused: her rifle is unknown";
		case ShotAct::kCount: break;
		}
		return "?";
	}

	Shot ShotEffect(proto::QuietWeapon a_w, const ShotTarget& a_t, float a_share, bool a_hasPost, bool a_clear, const Rules& a_r, const combat::HitRules& a_hit,
		const cqc::Rules& a_cqc, const combat::MeleeValues& a_melee)
	{
		Shot s;
		if (!a_t.present) {
			s.act = ShotAct::kGone;
			return s;
		}
		if (!a_t.hostile) {
			s.act = ShotAct::kNotHostile;  // the effect checks again: never a friend, whatever the pick said
			return s;
		}
		if (a_t.boss) {
			s.act = ShotAct::kBoss;
			return s;
		}
		if (!a_t.alive) {
			s.act = ShotAct::kDead;
			return s;
		}
		if (!a_hasPost) {
			s.act = ShotAct::kNoPost;
			return s;
		}
		if (!a_clear) {
			s.act = ShotAct::kBlocked;
			return s;
		}
		if (a_w != proto::kQuietLethal && a_w != proto::kQuietHeavy && a_w != proto::kQuietTranq) {
			s.act = ShotAct::kUnknownRifle;  // never lethal by default (review: an unknown id fell through to the lethal block)
			return s;
		}
		if (a_w == proto::kQuietTranq) {
			s.act = ShotAct::kSleep;
			s.hit.sleep = a_melee.chokeSleep;
			s.hit.sleepShare = a_r.tranqSleep;
			s.hit.nonLethal = true;
			s.hit.poise = 10.0f;
			s.hit.dmgLevel = 1;
			s.hit.quiet = true;
			if (a_cqc.hitCorrection > 0) {
				s.hit.damage = 1;
				s.hit.correction = std::min<std::uint16_t>(a_cqc.hitCorrection, 100);
			}
			return s;
		}
		const bool  shared = std::isfinite(a_share) && a_share > 0.0f;
		const float lives = shared ? std::min(a_share, 20.0f) * a_r.lethalShareMult : a_r.lethalFallback;
		if (lives > 0.0f && a_t.maxHp > 0) {
			const float progression = std::isfinite(a_hit.progression) && a_hit.progression > 0.0f ? a_hit.progression : 1.0f;
			const float v = lives * combat::ReferenceHp(a_t.maxHp, a_hit.scaling) * progression;
			s.hit.damage = std::clamp(static_cast<std::int32_t>(std::lround(v)), 1, std::max(1, a_hit.maxDamage));
		}
		s.hit.shared = shared;
		s.hit.landOrHp = true;  // MGSV's round hit: a bullet a turn or a shield swallowed still deals it
		s.hit.quiet = true;     // a sniper Elden Ring's enemies don't hear ([Quiet] shot_noise)
		if (a_w == proto::kQuietHeavy) {
			s.act = ShotAct::kHeavy;
			if (a_r.heavyPoise) {
				s.hit.poise = a_cqc.throwPoise;
				s.hit.dmgLevel = a_cqc.throwDmgLevel;
			} else {
				s.hit.poise = 40.0f;
				s.hit.dmgLevel = 2;
			}
		} else {
			s.act = ShotAct::kLethal;
			s.hit.poise = 40.0f;
			s.hit.dmgLevel = 2;
		}
		return s;
	}

	// ---- texts -------------------------------------------------------------------------------------------------------------
	std::string OrderText(std::uint32_t a_cmd, proto::QuietWeapon a_w)
	{
		switch (a_cmd) {
		case 1:
		case 2:
			return a_w == proto::kQuietTranq ? "Quiet: Fire (tranq enemy) -- the enemy you marked" : "Quiet: Fire (kill enemy) -- the enemy you marked";
		case 3:
			return "Quiet: Take aim -- she fires when you do";
		case 4:
			return "Quiet: Cover me -- not in Elden Ring yet";
		case 5:
			return "Quiet: Cease cover";
		case 6:
			return "Quiet: Shoot this -- not in Elden Ring yet";
		case 7:
			return "Quiet";
		default:
			return std::format("Quiet: order {}", a_cmd);
		}
	}

	std::string ShotText(std::uint32_t a_phase, std::uint32_t a_reason)
	{
		if (a_phase == proto::kQuietRefused) {
			return "Quiet: no target -- look at an enemy through your binoculars";
		}
		if (a_phase == proto::kQuietLost && a_reason == proto::kQuietLostMgsv) {
			return "Quiet: no shot (MGSV)";
		}
		return {};
	}
}
