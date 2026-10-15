#include "ProjectileLogic.h"

#include <algorithm>
#include <cmath>

namespace phantom::er::projectile
{
	namespace
	{
		// 3x3 as columns (the images of the local axes): v = c0 * x + c1 * y + c2 * z.
		Vec3 Apply(const Basis& a_b, const Vec3& a_v) { return a_b.x * a_v.x + a_b.y * a_v.y + a_b.z * a_v.z; }
		Vec3 ApplyT(const Basis& a_b, const Vec3& a_v) { return { Dot(a_b.x, a_v), Dot(a_b.y, a_v), Dot(a_b.z, a_v) }; }

		Vec3 Col(const float* a_m, int a_c) { return { a_m[a_c], a_m[4 + a_c], a_m[8 + a_c] }; }
		void SetCol(float* a_m, int a_c, const Vec3& a_v)
		{
			a_m[a_c] = a_v.x;
			a_m[4 + a_c] = a_v.y;
			a_m[8 + a_c] = a_v.z;
		}
	}

	Basis FlightBasis(const Vec3& a_dir)
	{
		Basis b;
		const float len = Length(a_dir);
		if (!std::isfinite(len) || len < 1e-4f) {
			return b;
		}
		b.z = a_dir * (1.0f / len);
		const Vec3 up = std::fabs(b.z.y) > 0.98f ? Vec3{ 0, 0, 1 } : Vec3{ 0, 1, 0 };
		b.x = Normalize(Cross(up, b.z));
		b.y = Cross(b.z, b.x);
		return b;
	}

	Template Learn(std::span<const proto::DrawRecord> a_draws, std::span<const float> a_matrices, const Vec3& a_dir)
	{
		Template    t;
		const Basis b = FlightBasis(a_dir);
		bool        haveOrigin = false;
		Vec3        origin;
		for (const auto& d : a_draws) {
			if (d.owner != proto::kOwnerProjectile || d.matrixCount == 0 || std::size_t(d.firstMatrix + d.matrixCount) * 12 > a_matrices.size()) {
				continue;
			}
			if (t.parts.size() >= kMaxParts) {
				return {};  // more than a grenade or a rocket is made of: not one
			}
			const float* m0 = a_matrices.data() + std::size_t(d.firstMatrix) * 12;
			if (!haveOrigin) {
				origin = { m0[3], m0[7], m0[11] };
				haveOrigin = true;
			}
			Part p;
			p.record = d;
			p.local.resize(std::size_t(d.matrixCount) * 12);
			for (std::uint32_t i = 0; i < d.matrixCount; ++i) {
				const float* m = m0 + std::size_t(i) * 12;
				float*       l = p.local.data() + std::size_t(i) * 12;
				for (int c = 0; c < 3; ++c) {
					SetCol(l, c, ApplyT(b, Col(m, c)));
				}
				const Vec3 at{ m[3], m[7], m[11] };
				if (!Finite(at)) {
					return {};
				}
				t.extent = std::max(t.extent, Length(at - origin));
				SetCol(l, 3, ApplyT(b, at - origin));
			}
			t.at = t.at + Vec3{ m0[3], m0[7], m0[11] };
			t.parts.push_back(std::move(p));
		}
		if (!t.parts.empty()) {
			t.at = t.at * (1.0f / static_cast<float>(t.parts.size()));
		}
		if (t.extent > kMaxExtent) {
			return {};
		}
		return t;
	}

	void Place(const Template& a_t, const Vec3& a_pos, const Vec3& a_dir, float a_spin, std::vector<proto::DrawRecord>& a_draws, std::vector<float>& a_matrices)
	{
		const Basis f = FlightBasis(a_dir);
		// The spin about the flight axis folded into the frame: x and y turn, z stays.
		const float c = std::cos(a_spin), s = std::sin(a_spin);
		Basis       b;
		b.x = f.x * c + f.y * s;
		b.y = f.y * c - f.x * s;
		b.z = f.z;
		for (const auto& p : a_t.parts) {
			auto r = p.record;
			r.firstMatrix = static_cast<std::uint32_t>(a_matrices.size() / 12);
			r.owner = proto::kOwnerProjectile;
			const std::size_t n = p.local.size() / 12;
			r.matrixCount = static_cast<std::uint32_t>(n);
			for (std::size_t i = 0; i < n; ++i) {
				const float* l = p.local.data() + i * 12;
				float        m[12];
				for (int k = 0; k < 3; ++k) {
					SetCol(m, k, Apply(b, Col(l, k)));
				}
				SetCol(m, 3, a_pos + Apply(b, Col(l, 3)));
				a_matrices.insert(a_matrices.end(), m, m + 12);
			}
			a_draws.push_back(r);
		}
	}

	Vec3 ToListDir(const float a_a[12], const Vec3& a_w) { return { Dot(Col(a_a, 0), a_w), Dot(Col(a_a, 1), a_w), Dot(Col(a_a, 2), a_w) }; }

	Vec3 ToListPoint(const float a_a[12], const Vec3& a_w) { return ToListDir(a_a, a_w - Col(a_a, 3)); }

	Vec3 FromListPoint(const float a_a[12], const Vec3& a_p)
	{
		return Col(a_a, 0) * a_p.x + Col(a_a, 1) * a_p.y + Col(a_a, 2) * a_p.z + Col(a_a, 3);
	}

	std::vector<std::uint64_t> Signature(const Template& a_t)
	{
		std::vector<std::uint64_t> keys;
		for (const auto& p : a_t.parts) {
			keys.push_back(p.record.mesh);
		}
		std::ranges::sort(keys);
		return keys;
	}

	bool Confirms(const Template& a_t0, std::uint64_t a_frame0, const Template& a_t1, std::uint64_t a_frame1, const Vec3& a_dir, float a_fps)
	{
		if (a_t0.parts.empty() || a_frame1 <= a_frame0 || !(a_fps > 1.0f) || Signature(a_t0) != Signature(a_t1)) {
			return false;
		}
		const auto frames = a_frame1 - a_frame0;
		if (frames < 3 || frames > 20) {
			return false;
		}
		const Vec3  moved = a_t1.at - a_t0.at;
		const float dist = Length(moved), dirLen = Length(a_dir);
		if (!std::isfinite(dist) || !std::isfinite(dirLen) || dirLen < 1e-4f) {
			return false;
		}
		const float seconds = static_cast<float>(frames) / a_fps;
		return dist >= kMinConfirmSpeed * seconds && Dot(moved, a_dir) >= 0.5f * dist * dirLen;
	}

	float Across(const Template& a_t, std::span<const Box> a_boxes)
	{
		bool  any = false;
		Vec3  lo, hi;
		auto  grow = [&](const Vec3& a_p) {
			if (!any) {
				lo = hi = a_p;
				any = true;
				return;
			}
			lo = { std::min(lo.x, a_p.x), std::min(lo.y, a_p.y), std::min(lo.z, a_p.z) };
			hi = { std::max(hi.x, a_p.x), std::max(hi.y, a_p.y), std::max(hi.z, a_p.z) };
		};
		for (std::size_t i = 0; i < a_t.parts.size(); ++i) {
			const auto& part = a_t.parts[i];
			const Box*  box = i < a_boxes.size() && a_boxes[i].valid ? &a_boxes[i] : nullptr;
			for (std::size_t m = 0; m + 12 <= part.local.size(); m += 12) {
				const float* l = part.local.data() + m;
				if (!box) {
					grow(Col(l, 3));
					continue;
				}
				for (int c = 0; c < 8; ++c) {
					const Vec3 corner{ (c & 1) ? box->max[0] : box->min[0], (c & 2) ? box->max[1] : box->min[1], (c & 4) ? box->max[2] : box->min[2] };
					grow(Col(l, 0) * corner.x + Col(l, 1) * corner.y + Col(l, 2) * corner.z + Col(l, 3));
				}
			}
		}
		return any ? Length(hi - lo) : 0.0f;
	}

	void Advance(Pose& a_pose, const Vec3& a_vel, float a_age, bool a_tumbles, float a_spinRate)
	{
		const bool  finite = Finite(a_vel) && std::isfinite(a_age);
		const float speed = finite ? Length(a_vel) : 0.0f;
		if (speed > 0.3f) {
			a_pose.heading = a_vel;
		} else if (!a_pose.have) {
			// Never seen moving (it was already lying there): on its side, level.
			const Vec3 flat{ finite ? a_vel.x : 0.0f, 0, finite ? a_vel.z : 0.0f };
			a_pose.heading = LengthXZ(flat) > 1e-3f ? flat : Vec3{ 0, 0, 1 };
		}
		// The spin accumulates while it moves (no jump when a bounce starts it again) and holds when it stops.
		if (a_pose.have && a_tumbles && speed > 0.5f && std::isfinite(a_spinRate) && a_age > a_pose.age) {
			a_pose.spin = std::fmod(a_pose.spin + (a_age - a_pose.age) * a_spinRate, 6.2831853f);
		}
		if (std::isfinite(a_age)) {
			a_pose.age = a_age;
		}
		a_pose.have = true;
	}

	void HandBack(std::vector<proto::DrawRecord>& a_draws, std::span<const float> a_matrices, bool a_keepNear)
	{
		for (auto& d : a_draws) {
			if (d.owner != proto::kOwnerProjectile || !a_keepNear || d.matrixCount == 0 || (std::size_t(d.firstMatrix) + 1) * 12 > a_matrices.size()) {
				continue;
			}
			const Vec3 at = Col(a_matrices.data() + std::size_t(d.firstMatrix) * 12, 3);
			if (Finite(at) && Length(at) < kHandBackRadius) {
				d.owner = proto::kOwnerPlayer;
			}
		}
		std::erase_if(a_draws, [](const proto::DrawRecord& a_d) { return a_d.owner == proto::kOwnerProjectile; });
	}

	int PickRelease(std::span<const Pending> a_pending, double a_nowMs, double a_maxAgeMs)
	{
		int best = -1;
		for (std::size_t i = 0; i < a_pending.size(); ++i) {
			const auto& p = a_pending[i];
			if (p.learned || a_nowMs - p.atMs > a_maxAgeMs || a_nowMs < p.atMs) {
				continue;
			}
			if (best < 0 || p.atMs >= a_pending[static_cast<std::size_t>(best)].atMs) {
				best = static_cast<int>(i);
			}
		}
		return best;
	}
}
