#include "Pacing.h"

#include "Config.h"
#include "Host.h"
#include "Link.h"
#include "Log.h"
#include "PacingProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <tuple>

namespace phantom::er::Pacing
{
	namespace
	{
		using pacing::History;

		struct DrawSnapshot
		{
			std::uint64_t                  frame{ 0 };
			std::vector<proto::DrawRecord> records;
			std::vector<float>             matrices;
		};

		History                  g_history;
		pacing::StepClock        g_clock, g_drawClock;
		Motion                   g_motion;
		double                   g_step{ 0 }, g_drawStep{ 0 };
		std::uint64_t            g_stepFrame{ 0 }, g_drawStepFrame{ 0 };  // the Elden Ring frame each was computed for
		std::deque<DrawSnapshot> g_lists;
		bool                     g_wasReady{ false };
		// Statistics for `pacing`: Elden Ring frames by mode, and the blend cost.
		std::uint64_t g_modes[5]{}, g_drawModes[5]{}, g_frames{ 0 }, g_fitted{ 0 };
		double        g_costMs{ 0 };
		bool          g_loggedFirst{ false };

		double NowMs() { return Link::QpcToMs(Link::Qpc()); }

		pacing::StepClock::Params ClockParams()
		{
			pacing::StepClock::Params p;
			p.fixedDelay = Cfg().smoothDelay;
			return p;
		}

		pacing::Sample FromGuest(const proto::GuestState& a_g)
		{
			pacing::Sample s;
			s.frame = a_g.frame;
			s.t = Link::QpcToMs(a_g.qpc);
			s.epoch = a_g.odoEpoch;
			std::copy(std::begin(a_g.odo), std::end(a_g.odo), s.odo);
			s.rootYaw = a_g.rootYaw;
			s.camRel = Vec3{ a_g.camPos[0], a_g.camPos[1], a_g.camPos[2] } - Vec3{ a_g.rootPos[0], a_g.rootPos[1], a_g.rootPos[2] };
			s.camQuat = pacing::Normalized({ a_g.camQuat[0], a_g.camQuat[1], a_g.camQuat[2], a_g.camQuat[3] });
			s.fovY = a_g.camFovY;
			return s;
		}

		bool Usable(const proto::GuestState& a_g)
		{
			return (a_g.flags & proto::kGuestReady) && !(a_g.flags & proto::kGuestLoading) && a_g.frame != 0;
		}

		void Reset()
		{
			g_history.Clear();
			g_clock.Reset();
			g_drawClock.Reset();
			g_lists.clear();
			g_motion = {};
		}

		// The newest acquired draw list, kept (the shared slot goes back to the guest at the next acquire).
		void KeepFrontList()
		{
			std::uint32_t bytes = 0;
			const auto*   base = Link::Get().FrontDrawBytes(bytes);
			const auto*   list = Link::Get().FrontDraw();
			if (!base || !list || list->drawCount > 4096 || list->matrixCount > (1u << 16)) {
				return;
			}
			if (!g_lists.empty() && list->frame <= g_lists.back().frame) {
				if (list->frame + 600 < g_lists.back().frame) {
					g_lists.clear();  // a restarted guest numbers its frames from 0 again
				} else {
					return;
				}
			}
			DrawSnapshot d;
			d.frame = list->frame;
			const auto* recs = reinterpret_cast<const proto::DrawRecord*>(base + sizeof(proto::DrawList));
			const auto* mats = reinterpret_cast<const float*>(base + sizeof(proto::DrawList) + list->drawCount * sizeof(proto::DrawRecord));
			d.records.assign(recs, recs + list->drawCount);
			d.matrices.assign(mats, mats + std::size_t(list->matrixCount) * 12);
			g_lists.push_back(std::move(d));
			while (g_lists.size() > 4) {
				g_lists.pop_front();
			}
		}

		// The two lists around a_step on the step timeline (b = a when there is only one usable); t from a to b.
		bool PickLists(double a_step, const DrawSnapshot*& a_a, const DrawSnapshot*& a_b, float& a_t, int& a_mode)
		{
			std::vector<std::pair<double, const DrawSnapshot*>> at;
			for (const auto& l : g_lists) {
				if (const auto s = g_history.StepOf(l.frame)) {
					at.emplace_back(*s, &l);
				}
			}
			if (at.empty()) {
				if (g_lists.empty()) {
					return false;
				}
				a_a = a_b = &g_lists.back();
				a_t = 1;
				a_mode = static_cast<int>(History::Mode::kHoldNewest);
				return true;
			}
			const auto& newest = at.back();
			if (a_step >= newest.first) {
				a_a = a_b = newest.second;
				a_t = 1;
				a_mode = static_cast<int>(History::Mode::kHoldNewest);
				if (at.size() >= 2) {
					const auto&  prev = at[at.size() - 2];
					const double span = newest.first - prev.first;
					if (span > 0 && span <= 4.0 && a_step - newest.first <= 0.5) {  // half a step at most: a pose is a guess
						a_a = prev.second;
						a_t = static_cast<float>(1.0 + (a_step - newest.first) / span);
						a_mode = static_cast<int>(History::Mode::kExtrapolate);
					}
				}
				return true;
			}
			if (a_step < at.front().first) {
				a_a = a_b = at.front().second;
				a_t = 0;
				a_mode = static_cast<int>(History::Mode::kHoldOldest);
				return true;
			}
			for (std::size_t i = at.size() - 1; i > 0; --i) {
				if (at[i - 1].first <= a_step) {
					const double span = at[i].first - at[i - 1].first;
					a_a = at[i - 1].second;
					a_b = at[i].second;
					a_t = span > 1e-9 ? static_cast<float>((a_step - at[i - 1].first) / span) : 1.0f;
					a_mode = static_cast<int>(History::Mode::kBlend);
					return true;
				}
			}
			return false;
		}

		// Records of the nearer list; each one's matrices blended with its match in the other list (same mesh range and
		// owner, the n-th such draw with the n-th) unless the two are more than a metre apart (a warp the walker took alone).
		void BlendLists(const DrawSnapshot& a_a, const DrawSnapshot& a_b, float a_t, std::vector<proto::DrawRecord>& a_recs, std::vector<float>& a_mats)
		{
			const DrawSnapshot& base = a_t < 0.5f ? a_a : a_b;
			const DrawSnapshot& other = a_t < 0.5f ? a_b : a_a;
			using Key = std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
			std::map<Key, const proto::DrawRecord*>                                                   inOther;
			std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t, std::uint32_t>, std::uint32_t> count;
			for (const auto& d : other.records) {
				inOther[{ d.mesh, d.startIndex, d.indexCount, d.owner, count[{ d.mesh, d.startIndex, d.indexCount, d.owner }]++ }] = &d;
			}
			count.clear();
			a_recs.clear();
			a_mats.clear();
			a_recs.reserve(base.records.size());
			a_mats.reserve(base.matrices.size());
			const std::size_t baseMats = base.matrices.size() / 12, otherMats = other.matrices.size() / 12;
			for (const auto& d : base.records) {
				const auto it = inOther.find({ d.mesh, d.startIndex, d.indexCount, d.owner, count[{ d.mesh, d.startIndex, d.indexCount, d.owner }]++ });
				if (d.matrixCount == 0 || std::size_t(d.firstMatrix) + d.matrixCount > baseMats) {
					continue;
				}
				proto::DrawRecord r = d;
				r.firstMatrix = static_cast<std::uint32_t>(a_mats.size() / 12);
				const float*      mb = base.matrices.data() + std::size_t(d.firstMatrix) * 12;
				const float*      mo = nullptr;
				if (it != inOther.end() && it->second->matrixCount == d.matrixCount && std::size_t(it->second->firstMatrix) + d.matrixCount <= otherMats) {
					mo = other.matrices.data() + std::size_t(it->second->firstMatrix) * 12;
					const float dx = mo[3] - mb[3], dy = mo[7] - mb[7], dz = mo[11] - mb[11];
					if (dx * dx + dy * dy + dz * dz > 1.0f) {
						mo = nullptr;
					}
				}
				const std::size_t at = a_mats.size();
				a_mats.resize(at + std::size_t(d.matrixCount) * 12);
				for (std::uint32_t m = 0; m < d.matrixCount; ++m) {
					float* out = a_mats.data() + at + std::size_t(m) * 12;
					if (!mo) {
						std::memcpy(out, mb + std::size_t(m) * 12, 48);
					} else if (&base == &a_a) {
						pacing::Blend34(mb + std::size_t(m) * 12, mo + std::size_t(m) * 12, a_t, out);
					} else {
						pacing::Blend34(mo + std::size_t(m) * 12, mb + std::size_t(m) * 12, a_t, out);
					}
				}
				a_recs.push_back(r);
			}
		}
	}

	void OnFrameBegin(const Frame& a_f)
	{
		const bool ready = a_f.guestReady && Usable(a_f.guest);
		if (!ready) {
			if (g_wasReady) {
				Reset();
			}
			g_wasReady = false;
			g_motion.valid = false;
			return;
		}
		g_wasReady = true;
		g_history.Add(FromGuest(a_f.guest));
		const double now = NowMs();
		const auto*  newest = g_history.Newest();
		const double target = g_history.StepAt(now);
		g_step = g_clock.Update(now, g_history.MsPerStep(), target, newest ? target - newest->step : -1.0, ClockParams());
		g_stepFrame = a_f.index;

		// Once per frame, here: the puppet (ChrIns_PrePhysics) and the camera (DrawParamUpdate) must show the same moment.
		g_motion.valid = false;
		if (!Cfg().smooth) {
			return;
		}
		const auto pick = g_history.At(g_step, Cfg().smoothExtrapolate);
		if (!pick.a) {
			return;
		}
		const auto s = pacing::Blend(*pick.a, *pick.b, pick.t);
		g_motion.valid = true;
		g_motion.frame = s.frame;
		g_motion.epoch = s.epoch;
		std::copy(std::begin(s.odo), std::end(s.odo), g_motion.odo);
		if (const int w = Cfg().smoothMotion; w >= 3 && pick.mode == History::Mode::kBlend) {
			// Snake's root moves in whole animation keys (2 or 3 per MGSV frame running): a line through the last frames
			// instead of the two around the moment (er/PacingLogic.h FitOdometer).
			if (pacing::FitOdometer(g_history, g_step, static_cast<std::size_t>(w), s.odo, 0.15, g_motion.odo)) {
				++g_fitted;
			}
		}
		g_motion.rootYaw = s.rootYaw;
		g_motion.camRel = s.camRel;
		g_motion.camQuat = s.camQuat;
		g_motion.fovY = s.fovY;
		++g_modes[static_cast<int>(pick.mode)];
		++g_frames;
		PacingProbe::OnPaced(a_f.index, g_step, pick.t, static_cast<std::uint32_t>(pick.mode));
		if (!g_loggedFirst && g_frames > 120) {
			g_loggedFirst = true;
			Log::Info("pacing: Elden Ring shows MGSV's motion blended {:.2f} steps behind its newest frame ({:.2f} ms per MGSV step)", g_clock.Delay(),
				g_history.MsPerStep());
		}
	}

	void Resample(const Frame& a_f)
	{
		// MGSV frames that arrive between two FrameBegins: one would be lost when two come (52-57 Hz vs 60, live).
		if (!g_wasReady || g_stepFrame != a_f.index) {
			return;
		}
		proto::GuestState g{};
		if (Link::Get().ReadGuestState(g) && Usable(g)) {
			g_history.Add(FromGuest(g));
		}
	}

	const Motion& Current() { return g_motion; }

	bool Draws(const Frame& a_f, std::vector<proto::DrawRecord>& a_records, std::vector<float>& a_matrices, std::uint64_t& a_frameOut)
	{
		if (!Cfg().render.smoothDraws || !Cfg().smooth || !g_wasReady || g_stepFrame != a_f.index) {
			return false;
		}
		const auto t0 = Link::Qpc();
		// A list published since FrameBegin is fresher to blend toward (its slot is ours until the next acquire).
		Link::Get().AcquireDraw();
		KeepFrontList();
		if (g_lists.empty()) {
			return false;
		}
		if (g_drawStepFrame != a_f.index) {
			const auto newest = g_history.StepOf(g_lists.back().frame);
			pacing::StepClock::Params p;  // always adaptive: how far the lists trail the states is MGSV's (live: one frame)
			p.minDelay = 0.0;
			g_drawStep = g_drawClock.Update(NowMs(), g_history.MsPerStep(), g_step, newest ? g_step - *newest : -1.0, p);
			g_drawStepFrame = a_f.index;
		}
		const DrawSnapshot* a = nullptr;
		const DrawSnapshot* b = nullptr;
		float               t = 0;
		int                 mode = 0;
		if (!PickLists(g_drawStep, a, b, t, mode)) {
			return false;
		}
		if (a == b) {
			a_records = a->records;
			a_matrices = a->matrices;
		} else {
			BlendLists(*a, *b, t, a_records, a_matrices);
		}
		a_frameOut = (t < 0.5f ? a : b)->frame;
		++g_drawModes[mode];
		g_costMs = g_costMs * 0.98 + Link::QpcToMs(Link::Qpc() - t0) * 0.02;
		PacingProbe::OnPacedDraws(a_f.index, g_drawStep, t, static_cast<std::uint32_t>(mode));
		return true;
	}

	std::string Describe()
	{
		const auto* n = g_history.Newest();
		return std::format("pacing: camera/puppet {} draws {} | {:.2f} ms per MGSV step, shown {:.2f} steps behind its newest frame (draws {:.2f} more), {} / {} snaps | "
						   "frames: {} blended ({} with the odometer fitted), {} extrapolated, {} held new, {} held old | draws: {} blended, {} extrapolated, {} held | blend {:.3f} ms/frame | newest MGSV "
						   "frame {} step {:.1f}, shown step {:.2f}, draws step {:.2f}",
			Cfg().smooth ? "smooth" : "newest", Cfg().render.smoothDraws ? "smooth" : "newest", g_history.MsPerStep(), g_clock.Delay(), g_drawClock.Delay(), g_clock.Snaps(),
			g_drawClock.Snaps(), g_modes[1], g_fitted, g_modes[2], g_modes[3], g_modes[4], g_drawModes[1], g_drawModes[2], g_drawModes[3] + g_drawModes[4], g_costMs, n ? n->frame : 0,
			n ? n->step : 0.0, g_step, g_drawStep);
	}

	std::string Command(const std::vector<std::string>& a_w)
	{
		// pacing [set smooth|draws on|off | set delay auto|<steps> | set extrapolate <steps>]
		if (a_w.size() >= 4 && a_w[1] == "set") {
			auto&       c = Cfg();
			const auto& v = a_w[3];
			const bool  on = v == "on" || v == "1" || v == "true";
			if (a_w[2] == "smooth") {
				c.smooth = on;
			} else if (a_w[2] == "draws") {
				c.render.smoothDraws = on;
			} else if (a_w[2] == "delay") {
				c.smoothDelay = v == "auto" ? -1.0f : static_cast<float>(std::strtod(v.c_str(), nullptr));
			} else if (a_w[2] == "extrapolate") {
				c.smoothExtrapolate = static_cast<float>(std::strtod(v.c_str(), nullptr));
			} else if (a_w[2] == "motion") {
				c.smoothMotion = std::atoi(v.c_str());
			} else {
				return "pacing set smooth|draws on|off | set delay auto|<steps> | set extrapolate <steps> | set motion <frames, 0 = off>";
			}
			return Describe();
		}
		return Describe();
	}
}
