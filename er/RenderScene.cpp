#include "RenderScene.h"

#include "Buddy.h"
#include "Calibration.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Link.h"
#include "Log.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Puppet.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <tuple>

namespace phantom::er::RenderScene
{
	namespace
	{
		constexpr std::size_t kHistory = 6;
		constexpr std::uint32_t kMaxDraws = 4096;
		constexpr std::uint32_t kMaxMatrices = 1u << 16;

		std::mutex                                        g_lock;
		std::atomic<bool>                                 g_hasMarkers{ false };
		std::atomic<bool>                                 g_hasNvg{ false };
		float                                             g_nvg{ 0 };  // night vision's fade (task thread)
		std::deque<std::shared_ptr<const RenderSnapshot>> g_history;
		std::uint64_t                                     g_captures{ 0 }, g_withDraws{ 0 }, g_badLists{ 0 };
		bool                                              g_loggedFirst{ false };
		float                                             g_buddyLift{ 0 };  // smoothed ground offset of the buddy's draws (m)
		std::uint32_t                                     g_tarnishedFrames{ 0 };  // consecutive frames the Tarnished was on screen

		Vec3 Row(const std::uint8_t* a_cam, int a_row)
		{
			const auto* f = reinterpret_cast<const float*>(a_cam + game::kCamMatrix + 16 * a_row);
			return { f[0], f[1], f[2] };
		}

		bool ReadCamera(RenderCamera& a_out)
		{
			const auto* cam = game::PersCam1();
			if (!cam) {
				return false;
			}
			a_out.right = Row(cam, 0);
			a_out.up = Row(cam, 1);
			a_out.fwd = Row(cam, 2) * Calibration::CamForwardRow();
			a_out.pos = Row(cam, 3);
			a_out.fovY = *reinterpret_cast<const float*>(cam + game::kCamFov);
			a_out.aspect = *reinterpret_cast<const float*>(cam + game::kCamAspect);
			a_out.nearZ = *reinterpret_cast<const float*>(cam + game::kCamNear);
			a_out.farZ = *reinterpret_cast<const float*>(cam + game::kCamFar);
			a_out.valid = Finite(a_out.right) && Finite(a_out.up) && Finite(a_out.fwd) && Finite(a_out.pos) && a_out.fovY > 0.05f && a_out.fovY < 3.0f &&
			              Length(a_out.fwd) > 0.5f;
			return a_out.valid;
		}

		// ER = pe + Ry(delta) * M * p + (0, root_offset_y, 0) for an anchor-relative MGSV point p: the
		// debug skeleton's mapping (er/DebugDraw.cpp pins the pose root, minus mgsv_root_offset_y, on the
		// feet), with the guest's anchorPos standing in for the pose root.
		void AnchorMatrix(const Anchor& a_a, float* a_m)
		{
			const Vec3 cx = RotY(a_a.delta, { a_a.axes.mirrorX, 0, 0 });
			const Vec3 cy{ 0, 1, 0 };
			const Vec3 cz = RotY(a_a.delta, { 0, 0, 1 });
			const auto& off = Cfg().render.debugOffset;
			const Vec3  t = a_a.pe + Vec3{ off[0], Cfg().mgsvRootOffsetY + off[1], off[2] };
			const float m[12] = { cx.x, cy.x, cz.x, t.x, cx.y, cy.y, cz.y, t.y, cx.z, cy.z, cz.z, t.z };
			std::memcpy(a_m, m, sizeof(m));
		}

		// The buddy walks on MGSV's flat sandbox: lift (or lower) its draws onto Elden Ring's ground under it (er/Buddy.cpp's
		// ray), smoothed so a rock or a step doesn't make it pop; not while Snake rides it (the puppet carries both).
		void GroundBuddy(RenderSnapshot& a_s, float a_dt)
		{
			const float target = !a_s.riding && Buddy::HasEstimate() ? std::clamp(Buddy::GroundOffset(), -1.5f, 1.5f) : 0.0f;
			g_buddyLift += (target - g_buddyLift) * std::min(1.0f, std::max(0.0f, a_dt) * 8.0f);
			if (std::fabs(g_buddyLift) < 0.005f) {
				return;
			}
			std::vector<std::uint8_t> moved(a_s.matrices.size() / 12, 0);
			for (const auto& d : a_s.draws) {
				if (d.owner != proto::kOwnerBuddy) {
					continue;
				}
				for (std::uint32_t m = d.firstMatrix; m < d.firstMatrix + d.matrixCount && m < moved.size(); ++m) {
					if (!moved[m]) {
						moved[m] = 1;
						a_s.matrices[std::size_t(m) * 12 + 7] += g_buddyLift;  // y: the anchor turns about y only
					}
				}
			}
		}

		// This frame's draw list: blended between MGSV's two lists around the moment shown (er/Pacing.h), else the newest.
		bool CopyDrawList(RenderSnapshot& a_s, const Frame& a_f)
		{
			std::vector<proto::DrawRecord> paced;
			const proto::DrawRecord*       recs = nullptr;
			std::uint32_t                  drawCount = 0, matrixCount = 0;
			if (Pacing::Draws(a_f, paced, a_s.matrices, a_s.guestFrame)) {
				recs = paced.data();
				drawCount = static_cast<std::uint32_t>(paced.size());
				matrixCount = static_cast<std::uint32_t>(a_s.matrices.size() / 12);
			} else {
				std::uint32_t bytes = 0;
				const auto*   base = Link::Get().FrontDrawBytes(bytes);
				const auto*   list = Link::Get().FrontDraw();
				if (!base || !list) {
					return false;
				}
				if (list->drawCount > kMaxDraws || list->matrixCount > kMaxMatrices) {
					++g_badLists;
					return false;
				}
				a_s.guestFrame = list->frame;
				recs = reinterpret_cast<const proto::DrawRecord*>(base + sizeof(proto::DrawList));
				const auto* mats = reinterpret_cast<const float*>(base + sizeof(proto::DrawList) + list->drawCount * sizeof(proto::DrawRecord));
				a_s.matrices.assign(mats, mats + std::size_t(list->matrixCount) * 12);
				drawCount = list->drawCount;
				matrixCount = list->matrixCount;
			}
			const auto& rc = Cfg().render;
			a_s.draws.reserve(drawCount);
			for (std::uint32_t i = 0; i < drawCount; ++i) {
				const auto& d = recs[i];
				if (d.matrixCount == 0 || std::uint64_t(d.firstMatrix) + d.matrixCount > matrixCount) {
					++g_badLists;
					continue;
				}
				if ((d.owner == proto::kOwnerPlayer && !rc.drawPlayer) || (d.owner == proto::kOwnerBuddy && !rc.drawBuddy) ||
					(d.owner >= proto::kOwnerVehicle && !rc.drawOther)) {
					continue;
				}
				a_s.draws.push_back(d);
			}
			return !a_s.draws.empty();
		}

		// Velocity needs where each draw was one Elden Ring frame ago: match it with the previous
		// snapshot's draw of the same mesh range and owner (the n-th such draw with the n-th).
		void LinkPrevious(RenderSnapshot& a_s, const RenderSnapshot* a_prev)
		{
			a_s.prevMatrices = a_s.matrices;
			a_s.prevValid.assign(a_s.draws.size(), 0);
			if (!a_prev || !a_prev->camera.valid) {
				return;
			}
			a_s.havePrev = true;
			a_s.prevCamera = a_prev->camera;
			std::memcpy(a_s.prevAnchor, a_prev->draw ? a_prev->anchor : a_s.anchor, sizeof(a_s.prevAnchor));
			if (!a_prev->draw) {
				return;
			}
			using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
			std::map<Key, std::size_t> before;
			auto                       key = [](const proto::DrawRecord& a_d, std::uint32_t a_n) { return Key{ a_d.mesh, a_d.startIndex, a_d.indexCount, a_d.owner, a_n }; };
			std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> count;
			for (std::size_t i = 0; i < a_prev->draws.size(); ++i) {
				const auto& d = a_prev->draws[i];
				before[key(d, count[{ d.mesh, d.startIndex, d.indexCount, d.owner }]++)] = i;
			}
			count.clear();
			for (std::size_t i = 0; i < a_s.draws.size(); ++i) {
				const auto& d = a_s.draws[i];
				const auto  it = before.find(key(d, count[{ d.mesh, d.startIndex, d.indexCount, d.owner }]++));
				if (it == before.end()) {
					continue;
				}
				const auto& p = a_prev->draws[it->second];
				if (p.matrixCount != d.matrixCount || std::size_t(p.firstMatrix + p.matrixCount) * 12 > a_prev->matrices.size()) {
					continue;
				}
				std::memcpy(a_s.prevMatrices.data() + std::size_t(d.firstMatrix) * 12, a_prev->matrices.data() + std::size_t(p.firstMatrix) * 12,
					std::size_t(d.matrixCount) * 48);
				a_s.prevValid[i] = 1;
			}
		}
	}

	void Capture(const Frame& a_f)
	{
		auto s = std::make_shared<RenderSnapshot>();
		s->erFrame = a_f.index;
		s->hour = game::TimeOfDay();
		// From the death until the respawn Snake is drawn at the puppet's last anchor, dissolving away
		// (or kept / cut, [Render] death_visual); the puppet itself has already stopped by then.
		const auto& rc = Cfg().render;
		const float dying = Puppet::DeathSeconds();
		if (dying >= 0.0f) {
			s->dissolve = rc.deathVisual == "keep" ? 0.0f
			            : rc.deathVisual == "fade" ? std::clamp(dying * 1000.0f / static_cast<float>(std::max(1, rc.deathFadeMs)), 0.0f, 1.0f)
			                                       : 1.0f;
		}
		if (rc.enabled && s->dissolve < 1.0f && ReadCamera(s->camera) && (Puppet::Active() || dying >= 0.0f) && a_f.guestReady && !a_f.loading) {
			const Anchor& a = Puppet::CurrentAnchor();
			AnchorMatrix(a, s->anchor);
			s->mirrored = a.axes.mirrorX < 0;
			s->riding = (a_f.guest.flags & proto::kGuestOnVehicle) != 0;
			s->firstPerson = (a_f.guest.flags & proto::kGuestFirstPerson) != 0;
			s->draw = CopyDrawList(*s, a_f);
			if (s->draw) {
				GroundBuddy(*s, a_f.dt);
			}
		}
		// The host's own world markers (er/RenderMarkers.h); their owners show none in menus, cutscenes, loads.
		s->markers = RenderMarkers::Collect();
		g_hasMarkers.store(!s->markers.empty(), std::memory_order_relaxed);
		// MGSV's night-vision goggles (protocol v10 kGuestNvg): on Elden Ring's frame while Snake plays with them on, faded in
		// and out like the goggles; not in cutscenes, loads or death.
		{
			const bool  want = rc.enabled && rc.nvg && Puppet::Active() && a_f.guestReady && (a_f.guest.flags & proto::kGuestNvg) && !a_f.cutscene &&
			                   !a_f.loading && !a_f.dead && !a_f.dying;
			const float step = rc.nvgFadeMs > 0 ? a_f.dt * 1000.0f / static_cast<float>(rc.nvgFadeMs) : 1.0f;
			const float was = g_nvg;
			g_nvg = std::clamp(g_nvg + (want ? step : -step), 0.0f, 1.0f);
			if ((was > 0.0f) != (g_nvg > 0.0f) || (was < 1.0f && g_nvg >= 1.0f)) {
				Log::Info("render: night vision {} (MGSV's goggles {})", g_nvg >= 1.0f ? "on" : (g_nvg > 0.0f ? "fading in" : "off"), want ? "on" : "off");
			}
			s->nvg = g_nvg;
			g_hasNvg.store(g_nvg > 0.0f, std::memory_order_relaxed);
		}
		if (s->camera.valid || ReadCamera(s->camera)) {
			// The Tarnished (visible while there is no puppet): where his chest is on screen.
			if (!Puppet::Active() && dying < 0.0f && a_f.inGame && !a_f.loading && a_f.player) {
				const auto& c = s->camera;
				const Vec3  d = game::Position(a_f.player) + Vec3{ 0, 1.3f, 0 } - c.pos;
				const Vec3  v{ Dot(d, Normalize(c.right)), Dot(d, Normalize(c.up)), Dot(d, Normalize(c.fwd)) };
				const float sy = 1.0f / std::tan(c.fovY * 0.5f);
				const float sx = sy / (c.aspect > 0.1f ? c.aspect : 16.0f / 9.0f);
				if (v.z > 0.3f && v.z < 20.0f) {
					s->tarnishedNdc[0] = v.x * sx / v.z;
					s->tarnishedNdc[1] = v.y * sy / v.z;
					s->tarnishedVisible = std::abs(s->tarnishedNdc[0]) < 0.8f && std::abs(s->tarnishedNdc[1]) < 0.8f;
				}
			}
		}
		// Only a Tarnished who has been on screen for a while: the one frame between Elden Ring's respawn and the
		// puppet taking over again showed him in his respawn state, whose light set (7 at a Stake of Marika) has no
		// light at all -- Snake went black (VERIFIED live).
		g_tarnishedFrames = s->tarnishedVisible ? g_tarnishedFrames + 1 : 0;
		s->tarnishedVisible = s->tarnishedVisible && g_tarnishedFrames > 90;
		if (s->draw && !g_loggedFirst) {
			g_loggedFirst = true;
			const auto& c = s->camera;
			Log::Info("render: first snapshot: {} draws, {} matrices, guest frame {}; camera pos ({:.2f}, {:.2f}, {:.2f}) fwd ({:.2f}, {:.2f}, {:.2f}) fov {:.3f} aspect {:.3f} "
					  "near {:.3f} far {:.1f}; anchor t ({:.2f}, {:.2f}, {:.2f})",
				s->draws.size(), s->matrices.size() / 12, s->guestFrame, c.pos.x, c.pos.y, c.pos.z, c.fwd.x, c.fwd.y, c.fwd.z, c.fovY, c.aspect, c.nearZ, c.farZ,
				s->anchor[3], s->anchor[7], s->anchor[11]);
		}
		PacingProbe::OnSnapshot(*s);
		std::scoped_lock lock(g_lock);
		LinkPrevious(*s, g_history.empty() ? nullptr : g_history.back().get());
		++g_captures;
		g_withDraws += s->draw ? 1 : 0;
		g_history.push_back(std::move(s));
		while (g_history.size() > kHistory) {
			g_history.pop_front();
		}
	}

	bool HasMarkers() { return g_hasMarkers.load(std::memory_order_relaxed); }
	bool HasNvg() { return g_hasNvg.load(std::memory_order_relaxed); }

	std::shared_ptr<const RenderSnapshot> ForPresent()
	{
		std::scoped_lock lock(g_lock);
		if (g_history.empty()) {
			return {};
		}
		const auto back = static_cast<std::size_t>(std::clamp(Cfg().render.latencyFrames, 0, static_cast<int>(kHistory) - 1));
		return g_history[g_history.size() - 1 - std::min(back, g_history.size() - 1)];
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_lock);
		const auto       newest = g_history.empty() ? nullptr : g_history.back();
		return std::format("snapshots {} ({} with draws, {} bad records) newest: ER frame {} guest frame {} draws {} matrices {}", g_captures, g_withDraws, g_badLists,
			newest ? newest->erFrame : 0, newest ? newest->guestFrame : 0, newest ? newest->draws.size() : 0, newest ? newest->matrices.size() / 12 : 0);
	}
}
