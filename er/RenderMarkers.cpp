#include "RenderMarkers.h"

#include <array>
#include <atomic>
#include <format>
#include <mutex>

namespace phantom::er::RenderMarkers
{
	namespace
	{
		// Set on the task thread (FrameBegin), collected on the task thread (Draw_Pre): the lock only guards against
		// a console command in between.
		std::mutex                                                          g_lock;
		std::array<std::vector<Marker>, static_cast<std::size_t>(Owner::kCount)> g_sets;
		std::atomic<std::uint64_t>                                          g_collected{ 0 }, g_markers{ 0 };

		void Corner(std::vector<Vertex>& a_out, const Vec3& a_p, float a_cx, float a_cy, const Marker& a_m, float a_viewZ)
		{
			Vertex v{};
			v.pos[0] = a_p.x;
			v.pos[1] = a_p.y;
			v.pos[2] = a_p.z;
			v.corner[0] = a_cx;
			v.corner[1] = a_cy;
			for (int i = 0; i < 4; ++i) {
				v.color[i] = a_m.color[i];
			}
			v.shape[0] = static_cast<float>(a_m.shape);
			v.shape[1] = a_m.inner;
			v.shape[2] = a_viewZ;
			v.shape[3] = a_m.slack;
			a_out.push_back(v);
		}
	}

	void Set(Owner a_owner, std::vector<Marker> a_markers)
	{
		std::scoped_lock lock(g_lock);
		g_sets[static_cast<std::size_t>(a_owner)] = std::move(a_markers);
	}

	void Clear()
	{
		std::scoped_lock lock(g_lock);
		for (auto& s : g_sets) {
			s.clear();
		}
	}

	std::vector<Marker> Collect()
	{
		std::scoped_lock    lock(g_lock);
		std::vector<Marker> out;
		for (const auto& s : g_sets) {
			out.insert(out.end(), s.begin(), s.end());
		}
		++g_collected;
		g_markers += out.size();
		return out;
	}

	std::vector<Vertex> Build(std::span<const Marker> a_markers, const Vec3& a_camPos, const Vec3& a_right, const Vec3& a_up)
	{
		std::vector<Vertex> out;
		out.reserve(a_markers.size() * 6);
		const Vec3 fwd = Normalize(Cross(a_right, a_up));
		for (const auto& m : a_markers) {
			if (!Finite(m.pos) || !(m.radius > 0.0f)) {
				continue;
			}
			Vec3 ax, ay, centre = m.pos;
			if (m.shape == Marker::Shape::kRing) {
				// On its surface, lifted a few cm toward the camera so the ground's depth doesn't hide it.
				const Vec3 n = Length(m.normal) > 0.5f ? Normalize(m.normal) : Vec3{ 0, 1, 0 };
				const Vec3 t = std::abs(n.y) < 0.95f ? Normalize(Cross(n, { 0, 1, 0 })) : Normalize(Cross(n, { 1, 0, 0 }));
				ax = t * m.radius;
				ay = Cross(n, t) * m.radius;
				centre = m.pos + n * 0.03f + Normalize(a_camPos - m.pos) * 0.05f;
			} else {
				ax = a_right * m.radius;
				ay = a_up * m.radius;
			}
			const float viewZ = Dot(centre - a_camPos, fwd);
			if (viewZ < 0.05f) {
				continue;  // behind the camera
			}
			const Vec3 c00 = centre - ax - ay, c10 = centre + ax - ay, c11 = centre + ax + ay, c01 = centre - ax + ay;
			Corner(out, c00, -1, -1, m, viewZ);
			Corner(out, c10, 1, -1, m, viewZ);
			Corner(out, c11, 1, 1, m, viewZ);
			Corner(out, c00, -1, -1, m, viewZ);
			Corner(out, c11, 1, 1, m, viewZ);
			Corner(out, c01, -1, 1, m, viewZ);
		}
		return out;
	}

	std::string Describe() { return std::format("markers: {} frames collected, {} markers", g_collected.load(), g_markers.load()); }
}
