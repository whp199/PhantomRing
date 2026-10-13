#include "PacingProbe.h"

#include "Host.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "RenderScene.h"

#include <atomic>
#include <cstdlib>
#include <format>
#include <mutex>

#include <windows.h>

namespace phantom::er::PacingProbe
{
	namespace
	{
		constexpr std::size_t kRows = 1u << 13;  // 136 s at 60 fps

		struct Row
		{
			std::uint64_t frame{ 0 };  // 0 = empty
			std::int64_t  fbQpc{ 0 };
			float         dt{ 0 };
			std::uint64_t gFrame{ 0 };
			std::int64_t  gQpc{ 0 };
			std::uint32_t gFlags{ 0 };
			double        odo[3]{};
			float         root[3]{};
			float         rootYaw{ 0 };
			float         gcam[3]{};
			float         gquat[4]{};
			std::uint64_t drawFrame{ 0 };
			std::int64_t  moveQpc{ 0 };
			float         peMove[3]{};
			float         step[3]{};
			std::int64_t  camQpc{ 0 };
			float         peCam[3]{};
			float         cam[3]{};
			float         fwd[3]{};
			float         rayLen{ -1 };
			float         pulled{ -1 };
			std::int64_t  snapQpc{ 0 };
			float         snapCam[3]{};
			float         snapFwd[3]{};
			float         snapAnchor[3]{};
			std::uint64_t snapDrawFrame{ 0 };
			std::int64_t  gbufQpc{ 0 };
			std::uint32_t gbufUses{ 0 };
			std::int64_t  presentQpc{ 0 };
			std::uint32_t presentUses{ 0 };
			float         viewErr{ -1 };
			double        pacedT{ 0 };
			double        pacedTDraw{ 0 };
			float         alpha{ -1 };
			float         alphaDraw{ -1 };
			std::uint32_t pacedFlags{ 0 };
		};

		std::mutex          g_lock;
		std::vector<Row>    g_rows;
		std::atomic<bool>   g_active{ false };
		std::uint64_t       g_until{ 0 };  // stop after this ER frame
		std::uint64_t       g_first{ 0 };
		std::uint64_t       g_recorded{ 0 };

		Row* At(std::uint64_t a_frame)
		{
			if (g_rows.empty() || !a_frame) {
				return nullptr;
			}
			auto& r = g_rows[a_frame % kRows];
			return r.frame == a_frame ? &r : nullptr;
		}

		void Put(float* a_out, const Vec3& a_v)
		{
			a_out[0] = a_v.x;
			a_out[1] = a_v.y;
			a_out[2] = a_v.z;
		}

		std::string Dump(const std::string& a_name)
		{
			std::string out =
				"frame,fb_qpc,dt,g_frame,g_qpc,g_flags,odo_x,odo_y,odo_z,root_x,root_y,root_z,root_yaw,gcam_x,gcam_y,gcam_z,gq_x,gq_y,gq_z,gq_w,draw_frame,"
				"move_qpc,pe_move_x,pe_move_y,pe_move_z,step_x,step_y,step_z,cam_qpc,pe_x,pe_y,pe_z,cam_x,cam_y,cam_z,fwd_x,fwd_y,fwd_z,ray_len,pulled,"
				"snap_qpc,snap_cam_x,snap_cam_y,snap_cam_z,snap_fwd_x,snap_fwd_y,snap_fwd_z,snap_anchor_x,snap_anchor_y,snap_anchor_z,snap_draw_frame,"
				"gbuf_qpc,gbuf_uses,present_qpc,present_uses,view_err,paced_t,paced_t_draw,alpha,alpha_draw,paced_flags\n";
			std::size_t n = 0;
			{
				std::scoped_lock lock(g_lock);
				for (std::uint64_t f = g_first; f && f <= g_first + kRows; ++f) {
					const Row* r = At(f);
					if (!r) {
						continue;
					}
					++n;
					out += std::format("{},{},{:.6f},{},{},{:#x},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.6f},{:.5f},{:.5f},{:.5f},{:.6f},{:.6f},{:.6f},{:.6f},{},", r->frame,
						r->fbQpc, r->dt, r->gFrame, r->gQpc, r->gFlags, r->odo[0], r->odo[1], r->odo[2], r->root[0], r->root[1], r->root[2], r->rootYaw, r->gcam[0], r->gcam[1],
						r->gcam[2], r->gquat[0], r->gquat[1], r->gquat[2], r->gquat[3], r->drawFrame);
					out += std::format("{},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.5f},{:.6f},{:.6f},{:.6f},{:.4f},{:.4f},",
						r->moveQpc, r->peMove[0], r->peMove[1], r->peMove[2], r->step[0], r->step[1], r->step[2], r->camQpc, r->peCam[0], r->peCam[1], r->peCam[2], r->cam[0],
						r->cam[1], r->cam[2], r->fwd[0], r->fwd[1], r->fwd[2], r->rayLen, r->pulled);
					out += std::format("{},{:.5f},{:.5f},{:.5f},{:.6f},{:.6f},{:.6f},{:.5f},{:.5f},{:.5f},{},{},{},{},{},{:.5f},{:.4f},{:.4f},{:.4f},{:.4f},{:#x}\n", r->snapQpc,
						r->snapCam[0], r->snapCam[1], r->snapCam[2], r->snapFwd[0], r->snapFwd[1], r->snapFwd[2], r->snapAnchor[0], r->snapAnchor[1], r->snapAnchor[2],
						r->snapDrawFrame, r->gbufQpc, r->gbufUses, r->presentQpc, r->presentUses, r->viewErr, r->pacedT, r->pacedTDraw, r->alpha, r->alphaDraw, r->pacedFlags);
				}
			}
			const std::wstring path = ModuleDir() + std::wstring(a_name.begin(), a_name.end());
			HANDLE             f = ::CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
			if (f == INVALID_HANDLE_VALUE) {
				return std::format("pacing probe: can't write {}", a_name);
			}
			DWORD written = 0;
			::WriteFile(f, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
			::CloseHandle(f);
			return std::format("pacing probe: {} rows -> {} ({} bytes)", n, a_name, out.size());
		}
	}

	bool Active() { return g_active.load(std::memory_order_relaxed); }

	void OnDrawAcquired(std::uint64_t a_erFrame, std::uint64_t a_drawFrame)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (auto* r = At(a_erFrame)) {
			r->drawFrame = a_drawFrame;
		}
	}

	void OnFrameBegin(const Frame& a_f)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (a_f.index > g_until) {
			g_active = false;
			Log::Info("pacing probe: done ({} frames)", g_recorded);
			return;
		}
		if (!g_first) {
			g_first = a_f.index;
		}
		auto& r = g_rows[a_f.index % kRows];
		r = Row{};
		r.frame = a_f.index;
		r.fbQpc = Link::Qpc();
		r.dt = a_f.dt;
		const auto& g = a_f.guest;
		r.gFrame = a_f.guestReady ? g.frame : 0;
		r.gQpc = g.qpc;
		r.gFlags = g.flags;
		for (int i = 0; i < 3; ++i) {
			r.odo[i] = g.odo[i];
			r.root[i] = g.rootPos[i];
			r.gcam[i] = g.camPos[i];
		}
		r.rootYaw = g.rootYaw;
		for (int i = 0; i < 4; ++i) {
			r.gquat[i] = g.camQuat[i];
		}
		++g_recorded;
	}

	void OnMoved(const Frame& a_f, const Vec3& a_pe, const Vec3& a_step)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (auto* r = At(a_f.index)) {
			r->moveQpc = Link::Qpc();
			Put(r->peMove, a_pe);
			Put(r->step, a_step);
		}
	}

	void OnCamera(const Frame& a_f, const Vec3& a_pe, const Vec3& a_camPos, const Vec3& a_camFwd, float a_rayLen, float a_pulledTo)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (auto* r = At(a_f.index)) {
			r->camQpc = Link::Qpc();
			Put(r->peCam, a_pe);
			Put(r->cam, a_camPos);
			Put(r->fwd, a_camFwd);
			r->rayLen = a_rayLen;
			r->pulled = a_pulledTo;
		}
	}

	void OnSnapshot(const RenderSnapshot& a_s)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (auto* r = At(a_s.erFrame)) {
			r->snapQpc = Link::Qpc();
			Put(r->snapCam, a_s.camera.pos);
			Put(r->snapFwd, a_s.camera.fwd);
			r->snapAnchor[0] = a_s.anchor[3];
			r->snapAnchor[1] = a_s.anchor[7];
			r->snapAnchor[2] = a_s.anchor[11];
			r->snapDrawFrame = a_s.draw ? a_s.guestFrame : 0;
		}
	}

	void OnPaced(std::uint64_t a_erFrame, double a_step, float a_alpha, std::uint32_t a_mode)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (auto* r = At(a_erFrame)) {
			r->pacedT = a_step;
			r->alpha = a_alpha;
			r->pacedFlags = (r->pacedFlags & ~0xFFu) | (a_mode & 0xFFu);
		}
	}

	void OnPacedDraws(std::uint64_t a_erFrame, double a_step, float a_alpha, std::uint32_t a_mode)
	{
		if (!Active()) {
			return;
		}
		std::scoped_lock lock(g_lock);
		if (auto* r = At(a_erFrame)) {
			r->pacedTDraw = a_step;
			r->alphaDraw = a_alpha;
			r->pacedFlags = (r->pacedFlags & 0xFFu) | ((a_mode & 0xFFu) << 8);
		}
	}

	void OnRender(int a_kind, std::uint64_t a_snapFrame, float a_value)
	{
		if (!Active()) {
			return;
		}
		const auto       now = Link::Qpc();
		std::scoped_lock lock(g_lock);
		auto*            r = At(a_snapFrame);
		if (!r) {
			return;
		}
		if (a_kind == 0) {
			r->gbufQpc = r->gbufQpc ? r->gbufQpc : now;
			++r->gbufUses;
		} else if (a_kind == 1) {
			r->presentQpc = r->presentQpc ? r->presentQpc : now;
			++r->presentUses;
		} else if (a_kind == 2) {
			r->viewErr = a_value;
		}
	}

	std::string Command(const std::vector<std::string>& a_w)
	{
		// pacing probe start [frames] | stop | dump [file] | (status)
		const std::string sub = a_w.size() > 2 ? a_w[2] : "";
		if (sub == "start") {
			const auto frames = a_w.size() > 3 ? std::strtoull(a_w[3].c_str(), nullptr, 10) : 1800ull;
			std::scoped_lock lock(g_lock);
			g_rows.assign(kRows, Row{});
			g_first = 0;
			g_recorded = 0;
			g_until = Host::Current().index + std::min<std::uint64_t>(frames ? frames : 1800, kRows - 1);
			g_active = true;
			return std::format("pacing probe: recording {} frames", std::min<std::uint64_t>(frames ? frames : 1800, kRows - 1));
		}
		if (sub == "stop") {
			g_active = false;
			return std::format("pacing probe: stopped after {} frames", g_recorded);
		}
		if (sub == "dump") {
			return Dump(a_w.size() > 3 ? a_w[3] : "PhantomRingHost.pacing.csv");
		}
		return std::format("pacing probe: {} ({} frames recorded); pacing probe start [frames] | stop | dump [file]", Active() ? "recording" : "idle", g_recorded);
	}
}
