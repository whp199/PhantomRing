#include "MarkingLogic.h"

#include "RenderMarkers.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace phantom::er::marking
{
	namespace
	{
		constexpr float kPopSeconds = 0.3f;
		constexpr float kPopScale = 0.6f;
		constexpr float kThroughWalls = 1.0e6f;  // depth slack (m): more than any scene, so no wall hides a mark (MGSV's way)
	}

	float ConeHalfAngle(const View& a_view, const Rules& a_rules)
	{
		const float minCone = a_rules.minConeDeg * std::numbers::pi_v<float> / 180.0f;
		const float fov = std::isfinite(a_view.fovY) && a_view.fovY > 0.0f ? a_view.fovY : 0.8f;
		return std::max(minCone, a_rules.coneShare * fov * 0.5f);
	}

	float OffAxis(const View& a_view, const Vec3& a_aim, const Rules& a_rules, float* a_dist)
	{
		const Vec3  d = a_aim - a_view.pos;
		const float dist = Length(d);
		if (a_dist) {
			*a_dist = dist;
		}
		if (dist <= std::max(a_rules.bodyRadius, 1e-3f)) {
			return 0.0f;  // the camera is inside him
		}
		const Vec3  f = Normalize(a_view.fwd);
		const float along = Dot(d, f);
		if (!(along > 0.0f)) {
			return std::numbers::pi_v<float>;
		}
		const float angle = std::acos(std::clamp(along / dist, -1.0f, 1.0f));
		const float body = std::asin(std::clamp(a_rules.bodyRadius / dist, 0.0f, 1.0f));
		return std::max(0.0f, angle - body);
	}

	Marks::Entry* Marks::Find(std::uint64_t a_handle)
	{
		auto it = std::find_if(e_.begin(), e_.end(), [&](const Entry& e) { return e.handle == a_handle; });
		return it == e_.end() ? nullptr : &*it;
	}

	const Marks::Entry* Marks::Find(std::uint64_t a_handle) const
	{
		auto it = std::find_if(e_.begin(), e_.end(), [&](const Entry& e) { return e.handle == a_handle; });
		return it == e_.end() ? nullptr : &*it;
	}

	void Marks::Trim(const Rules& a_rules)
	{
		while (Count() > std::max<std::size_t>(a_rules.maxMarked, 1)) {
			auto oldest = e_.end();
			for (auto it = e_.begin(); it != e_.end(); ++it) {
				if (it->marked && (oldest == e_.end() || it->order < oldest->order)) {
					oldest = it;
				}
			}
			e_.erase(oldest);
		}
	}

	std::vector<std::uint64_t> Marks::Update(const View& a_view, std::span<const Seen> a_seen, float a_dt, const Rules& a_rules, const LineOfSight& a_los)
	{
		std::vector<std::uint64_t> fresh;
		const float                dt = std::isfinite(a_dt) ? std::clamp(a_dt, 0.0f, 0.25f) : 0.0f;
		for (auto& e : e_) {
			if (e.marked) {
				e.age += dt;
			}
		}
		// Deaths (and a marked one back alive: Elden Ring's respawns at a grace keep their handles, MGSV's marks stay too).
		for (const auto& s : a_seen) {
			if (auto* e = Find(s.handle); e && e->marked) {
				e->deadFor = s.alive ? -1.0f : std::max(e->deadFor, 0.0f) + dt;
			}
		}
		// Who is looked at: in the cone and range, alive, not marked yet; the nearest the centre get the rays.
		std::vector<std::uint64_t> looked;
		if (a_view.binoculars && Finite(a_view.pos) && Finite(a_view.fwd) && Length(a_view.fwd) > 1e-3f) {
			const float cone = ConeHalfAngle(a_view, a_rules);
			struct Candidate
			{
				const Seen* s;
				float       off;
			};
			std::vector<Candidate> cands;
			for (const auto& s : a_seen) {
				if (!s.alive || !Finite(s.aim)) {
					continue;
				}
				if (const auto* e = Find(s.handle); e && e->marked) {
					continue;
				}
				Rules body = a_rules;
				if (s.body && std::isfinite(s.body->r)) {
					body.bodyRadius = std::max(a_rules.bodyRadius, s.body->r);
				}
				float       dist = 0;
				const float off = OffAxis(a_view, s.aim, body, &dist);
				if (off <= cone && dist <= a_rules.range) {
					cands.push_back({ &s, off });
				}
			}
			std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.off < b.off; });
			if (cands.size() > a_rules.maxRays) {
				cands.resize(a_rules.maxRays);
			}
			for (const auto& c : cands) {
				++rays_;
				if (a_los && !a_los(a_view.pos, *c.s)) {
					continue;
				}
				looked.push_back(c.s->handle);
			}
		}
		for (const auto h : looked) {
			auto* e = Find(h);
			if (!e) {
				e_.push_back({ h });
				e = &e_.back();
			}
			e->dwell += dt;
			if (e->dwell >= a_rules.dwell) {
				e->marked = true;
				e->age = 0;
				e->deadFor = -1;
				e->order = next_++;
				fresh.push_back(h);
			}
		}
		// Not looked at: the time looked so far ebbs away (twice as fast); a dead mark that has faded is forgotten.
		std::erase_if(e_, [&](Entry& e) {
			if (e.marked) {
				return e.deadFor >= std::max(a_rules.fade, 0.0f);
			}
			if (std::find(looked.begin(), looked.end(), e.handle) == looked.end()) {
				e.dwell -= 2.0f * dt;
			}
			return e.dwell <= 0.0f;
		});
		Trim(a_rules);
		return fresh;
	}

	bool RayClear(const Vec3& a_from, const Vec3& a_to, const Vec3& a_hit, const std::optional<combat::Capsule>& a_body)
	{
		if (!Finite(a_hit)) {
			return true;  // nothing usable came back: as no hit
		}
		if (Length(a_hit - a_from) >= Length(a_to - a_from) - kLosSlack) {
			return true;
		}
		return a_body && combat::CapsuleDistance(*a_body, a_hit) <= kLosBodySlack;
	}

	bool LoadWatch::Update(bool a_inWorld, std::uintptr_t a_player, std::uint64_t a_nowMs)
	{
		if (!a_inWorld) {
			if (!loading_) {
				loading_ = true;
				since_ = a_nowMs;
			}
			return false;
		}
		bool fresh = false;
		if (loading_) {
			loading_ = false;
			fresh = a_nowMs - since_ >= kMinLoadMs;
		}
		if (a_player && a_player != player_) {
			fresh = fresh || player_ != 0;
			player_ = a_player;
		}
		return fresh;
	}

	bool Marks::Marked(std::uint64_t a_handle) const
	{
		const auto* e = Find(a_handle);
		return e && e->marked;
	}

	float Marks::Opacity(std::uint64_t a_handle, float a_fade) const
	{
		const auto* e = Find(a_handle);
		if (!e || !e->marked) {
			return 0.0f;
		}
		if (e->deadFor < 0.0f) {
			return 1.0f;
		}
		return a_fade > 0.0f ? std::clamp(1.0f - e->deadFor / a_fade, 0.0f, 1.0f) : 0.0f;
	}

	float Marks::Age(std::uint64_t a_handle) const
	{
		const auto* e = Find(a_handle);
		return e && e->marked ? e->age : -1.0f;
	}

	float Marks::Dwell(std::uint64_t a_handle) const
	{
		const auto* e = Find(a_handle);
		return e && !e->marked ? e->dwell : 0.0f;
	}

	bool Marks::Mark(std::uint64_t a_handle, const Rules& a_rules)
	{
		auto* e = Find(a_handle);
		if (e && e->marked) {
			return false;
		}
		if (!e) {
			e_.push_back({ a_handle });
			e = &e_.back();
		}
		e->marked = true;
		e->age = 0;
		e->deadFor = -1;
		e->order = next_++;
		Trim(a_rules);
		return true;
	}

	void Marks::Forget(std::uint64_t a_handle)
	{
		std::erase_if(e_, [&](const Entry& e) { return e.handle == a_handle; });
	}

	void Marks::Clear() { e_.clear(); }

	std::size_t Marks::Count() const
	{
		return static_cast<std::size_t>(std::count_if(e_.begin(), e_.end(), [](const Entry& e) { return e.marked; }));
	}

	std::size_t Marks::Looking() const { return e_.size() - Count(); }

	std::string DistanceText(float a_metres)
	{
		if (!std::isfinite(a_metres)) {
			return {};
		}
		const long m = std::lround(std::clamp(a_metres, 0.0f, 9999.0f));
		return std::to_string(m) + "m";
	}

	std::vector<std::uint8_t> Glyphs(std::string_view a_text)
	{
		std::vector<std::uint8_t> out;
		for (const char c : a_text) {
			if (c >= '0' && c <= '9') {
				out.push_back(static_cast<std::uint8_t>(c - '0'));
			} else if (c == 'm') {
				out.push_back(10);
			}
		}
		return out;
	}

	void Build(std::vector<Marker>& a_out, const Camera& a_cam, const Vec3& a_at, std::string_view a_text, bool a_hostile, float a_opacity, float a_age,
		const Style& a_style)
	{
		const float viewZ = Dot(a_at - a_cam.pos, a_cam.fwd);
		if (!Finite(a_at) || !(viewZ > 0.1f) || !(a_opacity > 0.0f)) {
			return;
		}
		const float fov = std::isfinite(a_cam.fovY) && a_cam.fovY > 0.01f ? a_cam.fovY : 0.8f;
		const float screen = 2.0f * viewZ * std::tan(fov * 0.5f);  // the screen's height in metres at his depth
		const float pop = a_age >= 0.0f && a_age < kPopSeconds ? 1.0f + kPopScale * (1.0f - a_age / kPopSeconds) : 1.0f;
		const float base = a_style.size * screen;  // the triangle's half height once it has settled
		const float tri = base * pop;

		Marker t;
		t.shape = Marker::Shape::kTriangle;
		t.pos = a_at + a_cam.up * tri;  // its tip on a_at
		t.radius = tri;
		t.slack = kThroughWalls;
		const float* c = a_hostile ? a_style.hostile : a_style.other;
		std::copy_n(c, 4, t.color);
		t.color[3] *= std::clamp(a_opacity, 0.0f, 1.0f);
		a_out.push_back(t);

		const auto glyphs = Glyphs(a_text);
		if (glyphs.empty()) {
			return;
		}
		// No pop: the number stays put over where the triangle settles (the popping triangle overlaps it for those 0.3 s).
		const float g = a_style.textSize * base;
		const float advance = 1.15f * g;
		const Vec3  centre = a_at + a_cam.up * (2.0f * base + 1.3f * g);
		const float first = -0.5f * advance * static_cast<float>(glyphs.size() - 1);
		for (std::size_t i = 0; i < glyphs.size(); ++i) {
			Marker m;
			m.shape = Marker::Shape::kGlyph;
			m.inner = static_cast<float>(glyphs[i]);
			m.pos = centre + a_cam.right * (first + advance * static_cast<float>(i));
			m.radius = g;
			m.slack = kThroughWalls;
			std::copy_n(a_style.text, 4, m.color);
			m.color[3] *= std::clamp(a_opacity, 0.0f, 1.0f);
			a_out.push_back(m);
		}
	}
}
