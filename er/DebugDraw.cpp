#include "DebugDraw.h"

#include "Buddy.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Link.h"
#include "Log.h"
#include "Puppet.h"

#include <windows.h>

#include <vector>

namespace phantom::er::DebugDraw
{
	namespace
	{
		struct Tracer
		{
			Vec3          from, to;
			bool          hit, buddy;
			std::uint64_t until;
		};
		std::vector<Tracer> g_tracers;
		proto::PoseBlock    g_pose;  // 0x2260 bytes: static, not on the task's stack
		bool                g_loggedNoDrawer{ false };

		// Model-space point -> MGSV world with PoseBlock.root (row-major, translation in [12..14],
		// i.e. row-vector convention p * M).
		Vec3 RootTransform(const float* m, const float* p)
		{
			return { p[0] * m[0] + p[1] * m[4] + p[2] * m[8] + m[12], p[0] * m[1] + p[1] * m[5] + p[2] * m[9] + m[13],
				p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14] };
		}

		void DrawSkeleton()
		{
			if (!Link::Get().ReadPose(g_pose) || g_pose.boneCount == 0) {
				return;
			}
			// Pin the skeleton's own root on the puppet's feet: same anchor, pose-frame root.
			Anchor a = Puppet::CurrentAnchor();
			a.pm = { g_pose.root[12], g_pose.root[13] - Cfg().mgsvRootOffsetY, g_pose.root[14] };
			game::SetDrawColor(0.1f, 1.0f, 0.2f, 1.0f);
			for (std::uint32_t i = 0; i < g_pose.boneCount; ++i) {
				const int parent = g_pose.parent[i];
				if (parent == -2) {
					// The guest couldn't read the hierarchy (PoseBlock parent -2): a short tick per joint.
					const Vec3 joint = a.PointToEr(RootTransform(g_pose.root, g_pose.bones[i].pos));
					game::DrawLine(joint, joint + Vec3{ 0, 0.04f, 0 });
					continue;
				}
				if (parent < 0 || static_cast<std::uint32_t>(parent) >= g_pose.boneCount) {
					continue;
				}
				const Vec3 child = a.PointToEr(RootTransform(g_pose.root, g_pose.bones[i].pos));
				const Vec3 par = a.PointToEr(RootTransform(g_pose.root, g_pose.bones[parent].pos));
				game::DrawLine(par, child);
			}
		}

		void DrawFacing(const Frame& a_f)
		{
			const auto& a = Puppet::CurrentAnchor();
			const float h = MgsvHeading(a_f.guest.rootYaw, a.axes) + a.delta;
			const Vec3  base = a.pe + Vec3{ 0, 0.05f, 0 };
			game::SetDrawColor(1.0f, 0.9f, 0.1f, 1.0f);
			game::DrawLine(base, base + Vec3{ std::sin(h), 0, std::cos(h) });
		}
	}

	bool Available() { return game::EzDraw() != nullptr; }

	void AddTracer(const Vec3& a_from, const Vec3& a_to, bool a_hit, bool a_buddy)
	{
		if (g_tracers.size() < 256) {
			g_tracers.push_back({ a_from, a_to, a_hit, a_buddy, ::GetTickCount64() + static_cast<std::uint64_t>(Cfg().tracerSeconds * 1000.0f) });
		}
	}

	void Draw(const Frame& a_f)
	{
		if (!Cfg().debugDraw) {
			return;
		}
		if (!Available()) {
			if (!g_loggedNoDrawer && Puppet::Active()) {
				g_loggedNoDrawer = true;
				Log::Warn("debug draw: RendMan has no CSEzDraw; skeleton/tracers not drawn");
			}
			return;
		}
		if (Puppet::Active() && a_f.guestReady) {
			if (Cfg().drawSkeleton) {
				DrawSkeleton();
				DrawFacing(a_f);
			}
			if (Cfg().drawBuddy) {
				Buddy::Draw();
			}
		}
		const auto now = ::GetTickCount64();
		std::erase_if(g_tracers, [now](const Tracer& t) { return t.until < now; });
		if (Cfg().drawTracers) {
			for (const auto& t : g_tracers) {
				game::SetDrawColor(t.hit ? 1.0f : 0.6f, t.buddy ? 0.5f : 0.1f, 0.1f, 1.0f);
				game::DrawLine(t.from, t.to);
			}
		}
	}
}
