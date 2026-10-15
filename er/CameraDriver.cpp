#include "CameraDriver.h"

#include "Calibration.h"
#include "CameraCollision.h"
#include "Config.h"
#include "Host.h"
#include "Log.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Puppet.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace phantom::er::CameraDriver
{
	namespace
	{
		float g_maxDistance{ 0 };
		bool  g_driving{ false };
		bool  g_loggedFirst{ false };
		bool  g_loggedCompass{ false };
		camcol::Smoother g_collision;  // in at once, out eased (er/CameraCollision.h)
		float            g_feetY{ 0 };  // the puppet's feet height as the camera follows it (eased)
		bool             g_feetYValid{ false };
		std::uint64_t    g_feetYFrame{ 0 };
		// Snake's jump ([Puppet] jump_camera_hold): the camera's height over his root before it, held while the puppet is in
		// the air (the guest lifts Snake in MGSV and he falls there: MGSV's camera follows that, Elden Ring's must not bob).
		float            g_camRelY{ 0 };
		bool             g_camRelYValid{ false };
		// The camera as last mapped (DrawParamUpdate), for the compass write early in the next frame.
		struct Rows
		{
			Vec3          right, up, fwd, pos;
			float         fov{ 0 };  // 0 = leave the target's
			bool          valid{ false };
			std::uint64_t frame{ 0 };
		};
		Rows g_last;

		void WriteRow(std::uint8_t* a_cam, int a_row, const Vec3& a_v)
		{
			// Keep the w the game keeps in each row (0 for directions, 1 for the position).
			auto* f = reinterpret_cast<float*>(a_cam + game::kCamMatrix + 16 * a_row);
			f[0] = a_v.x;
			f[1] = a_v.y;
			f[2] = a_v.z;
		}

		void WriteRows(std::uint8_t* a_to, const Rows& a_r)
		{
			WriteRow(a_to, 0, a_r.right);
			WriteRow(a_to, 1, a_r.up);
			WriteRow(a_to, 2, a_r.fwd);
			WriteRow(a_to, 3, a_r.pos);
			if (a_r.fov > 0) {
				*reinterpret_cast<float*>(a_to + game::kCamFov) = a_r.fov;
			}
		}
	}

	void Apply(const Frame& a_f)
	{
		g_driving = false;
		if (!Cfg().camera || !Puppet::Active() || !a_f.guestReady || !a_f.player) {
			g_maxDistance = 0;
			return;
		}
		auto* cam = game::PersCam1();
		if (!cam) {
			return;
		}
		const auto& g = a_f.guest;
		const auto& anchor = Puppet::CurrentAnchor();
		Quat        q{ g.camQuat[0], g.camQuat[1], g.camQuat[2], g.camQuat[3] };
		const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		if (!(n > 0.5f && n < 2.0f)) {
			return;  // no camera from the guest yet
		}
		q = { q.x / n, q.y / n, q.z / n, q.w / n };
		// Where the camera is relative to Snake's root (MGSV world) and its fov: the paced blend of MGSV's frames around the
		// moment this Elden Ring frame shows (er/Pacing.h), else the newest frame. The puppet's feet (anchor.pe) moved by the
		// same moment's odometer, so the camera keeps its place relative to Snake and the world moves evenly under both.
		Vec3        camRel = Vec3{ g.camPos[0], g.camPos[1], g.camPos[2] } - Vec3{ g.rootPos[0], g.rootPos[1], g.rootPos[2] };
		float       fovY = g.camFovY;
		const auto& paced = Pacing::Current();
		if (paced.valid) {
			q = paced.camQuat;
			camRel = paced.camRel;
			fovY = paced.fovY;
		}
		if (Cfg().jumpCameraHold && Puppet::Jumping() && g_camRelYValid) {
			camRel.y = g_camRelY;  // as before the jump (UNVERIFIED live)
		} else {
			g_camRelY = camRel.y;
			g_camRelYValid = std::isfinite(camRel.y);
		}

		const Vec3 fwd = Normalize(anchor.DirToEr(Rotate(q, { 0, 0, anchor.axes.mgsvCamForward })));
		Vec3       up = anchor.DirToEr(Rotate(q, { 0, 1, 0 }));
		up = Normalize(up - fwd * Dot(up, fwd));
		const Vec3 rowFwd = fwd * Calibration::CamForwardRow();
		const Vec3 right = Normalize(Cross(up, rowFwd)) * Calibration::CamHandedness();
		// = anchor.PointToEr(camPos): pe + R(camPos - (root - root offset)), except that the feet's height is eased: the
		// kinematic puppet snaps onto Elden Ring's ground (a step, a rock) in one frame, and the camera must not hop
		// with it (Elden Ring's own camera eases too). Snake stays on the ground; a jump of 2 m (a warp, a fall's end
		// far below) is followed at once.
		Vec3 feet = anchor.pe;
		if (const float tau = Cfg().verticalSmoothing; tau > 0.0f && g_feetYValid && std::fabs(feet.y - g_feetY) < 2.0f && a_f.index == g_feetYFrame + 1) {
			g_feetY += (feet.y - g_feetY) * (1.0f - std::exp(-std::clamp(a_f.dt, 0.0f, 0.1f) / tau));
			feet.y = g_feetY;
		} else {
			g_feetY = feet.y;
		}
		g_feetYValid = true;
		g_feetYFrame = a_f.index;
		Vec3       pos = feet + anchor.DirToEr(camRel + Vec3{ 0, Cfg().mgsvRootOffsetY, 0 });
		if (!Finite(fwd) || !Finite(up) || !Finite(right) || !Finite(pos)) {
			return;
		}

		g_maxDistance = 0;
		float rayLen = -1.0f;
		if (Cfg().cameraCollision) {
			// From Snake's eye -- no higher than the camera when MGSV lowers it (crouched, prone), so the ray doesn't dive
			// into the slope behind him -- to the camera, plus rays to points collision_radius around the camera (its near
			// plane needs room, and a thin trunk then pulls it in for all the frames it crosses, not every other one).
			const float eyeY = Puppet::RidingHorse() ? Cfg().eyeHeightHorse :
			                   Puppet::Riding()      ? Cfg().eyeHeightVehicle :
			                                           std::clamp(pos.y - anchor.pe.y, 0.6f, Cfg().eyeHeight);
			const Vec3  eye = anchor.pe + Vec3{ 0, eyeY, 0 };
			const Vec3  seg = pos - eye;
			const float len = Length(seg);
			rayLen = len;
			float nearest = std::numeric_limits<float>::infinity();
			if (len > 0.05f) {
				const Vec3  dir = seg * (1.0f / len);
				const float r = Cfg().collisionRadius;
				const Vec3  ends[5] = { pos, pos + right * r, pos - right * r, pos + up * r, pos - up * r };
				for (int i = 0; i < (r > 0.0f ? 5 : 1); ++i) {
					Vec3 hit;
					if (game::CastRay(eye, ends[i] - eye, hit, a_f.player.chr, Cfg().rayFilter)) {
						nearest = std::min(nearest, Dot(hit - eye, dir));
					}
				}
			}
			camcol::Params cp;
			cp.margin = Cfg().collisionMargin;
			cp.easeOutSeconds = Cfg().collisionEaseOut;
			const float allowed = g_collision.Update(nearest, len, a_f.dt, cp);
			if (allowed < len && len > 0.05f) {
				pos = eye + seg * (allowed / len);
				g_maxDistance = allowed;
			}
		} else {
			g_collision.Reset();
		}

		g_last = { right, up, rowFwd, pos, (fovY > 0.1f && fovY < 3.0f) ? fovY * Cfg().fovScale + Cfg().fovOffset : 0.0f, true, a_f.index };
		WriteRows(cam, g_last);
		// Elden Ring's own player camera keeps running its follow logic for the hidden Tarnished and spins (live: its
		// rows turned 180 deg between samples 0.3 s apart); the logic that reads it (which distant enemies animate every
		// frame, 3D sound) follows it. Same CSCam layout as pers_cam_1, same space (ER copies one into the other). Written
		// after CameraStep, so what reads it until ER's next refresh sees Snake's camera; the HUD compass reads it after
		// that refresh and gets it again from ApplyCompass().
		if (Cfg().syncChrCam) {
			if (auto* chr = game::ChrCamObject(); chr && chr != cam) {
				WriteRows(chr, g_last);
			}
		}
		if (Cfg().writeNear && g.camNear > 0.01f && g.camNear < 5.0f) {
			*reinterpret_cast<float*>(cam + game::kCamNear) = g.camNear;
		}
		g_driving = true;
		PacingProbe::OnCamera(a_f, anchor.pe, pos, fwd, rayLen, g_maxDistance > 0 ? g_maxDistance : -1.0f);
		if (!g_loggedFirst) {
			g_loggedFirst = true;
			Log::Info("camera: driving pers_cam_1 ({}) from MGSV: pos ({:.2f}, {:.2f}, {:.2f}) fwd ({:.2f}, {:.2f}, {:.2f}) fov {:.3f}", game::RttiName(cam), pos.x, pos.y,
				pos.z, fwd.x, fwd.y, fwd.z, g.camFovY);
		}
	}

	void ApplyCompass()
	{
		// Only a fresh camera from a driving task: a stale one (MGSV gone, puppet off) is left to Elden Ring.
		if (!Cfg().syncCompassCam || !g_driving || !g_last.valid || g_last.frame + 2 < Host::Current().index) {
			return;
		}
		if (auto* chr = game::ChrCamObject()) {
			WriteRows(chr, g_last);
			if (!g_loggedCompass) {
				g_loggedCompass = true;
				Log::Info("camera: compass follows MGSV's camera (chr_cam {} rewritten before ER's compass update)", static_cast<void*>(chr));
			}
		}
	}

	float MaxDistance() { return g_maxDistance; }
	bool  Driving() { return g_driving; }
}
