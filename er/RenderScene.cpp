#include "RenderScene.h"

#include "Buddy.h"
#include "BuddyGuard.h"
#include "Calibration.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Ini.h"
#include "Link.h"
#include "Log.h"
#include "Pacing.h"
#include "PacingProbe.h"
#include "Projectiles.h"
#include "Puppet.h"
#include "WorldShift.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
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
		// While Snake dies (Puppet::DeathSeconds() >= 0): how many of his draws the last capture had, and why none (nullptr: drawn).
		std::atomic<std::uint32_t> g_dyingDraws{ 0 };
		std::atomic<const char*>   g_dyingWhy{ nullptr };
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
		Vec3                                              g_buddyShift;      // smoothed horizontal move (Buddy::DrawShift, ER space)
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

		// ---- (0.8.0 polish) D-Dog's drawn body guarded: er/BuddyGuard.h StepGuard on the dog's draws as Elden Ring shows them -----------
		buddyguard::StepGuard      g_drawGuard;
		float                      g_groundTarget{ 0 };   // the drawn buddy's lift and floor shift to smooth toward (GroundBuddy)
		Vec3                       g_groundShiftTarget;
		bool                       g_guardFromPub{ false };  // the draw guard watches the published place (else the draws' estimate)
		// `buddy trace on|off` (0.8.0 integration): one CSV line per frame D-Dog is drawn -- PhantomRingBuddyTrace.csv next to the DLL
		std::atomic<int>           g_traceWant{ 0 };      // +1 open, -1 close (the task thread does it)
		std::FILE*                 g_trace{ nullptr };
		std::uint64_t              g_traceRows{ 0 };
		std::uint64_t              g_guardNonce{ 0 };
		ULONGLONG                  g_guardDrawsAt{ 0 };   // the last frame the dog's draws were there
		Vec3                       g_guardPe;             // Snake's Elden Ring feet then (a new place: the guard starts over)
		bool                       g_guardPeValid{ false };
		std::uint32_t              g_guardEpoch{ 0 };     // Puppet::StartEpoch then (the puppet started anew: the guard starts over)
		ULONGLONG                  g_guardLogAt{ 0 };
		std::mutex                 g_guardStatsLock;
		struct GuardStats
		{
			std::array<std::uint64_t, static_cast<std::size_t>(buddyguard::Act::kCount)> acts{};
			std::uint64_t frames{ 0 }, rawOver{ 0 }, drawnOver{ 0 }, resets{ 0 }, staged{ 0 };
			float         rawMax{ 0 }, drawnMax{ 0 }, jumpMax{ 0 };
			std::uint32_t heldMaxMs{ 0 };
		} g_guardStats;

		Vec3 Apply(const float* a_m, const Vec3& a_p)
		{
			return { a_m[0] * a_p.x + a_m[1] * a_p.y + a_m[2] * a_p.z + a_m[3], a_m[4] * a_p.x + a_m[5] * a_p.y + a_m[6] * a_p.z + a_m[7],
				a_m[8] * a_p.x + a_m[9] * a_p.y + a_m[10] * a_p.z + a_m[11] };
		}

		// a_out = a_a * a_b (3x4 affine, row-major: the 4th row is 0 0 0 1).
		void Compose(const float* a_a, const float* a_b, float* a_out)
		{
			float r[12];
			for (int i = 0; i < 3; ++i) {
				for (int j = 0; j < 4; ++j) {
					r[i * 4 + j] = a_a[i * 4 + 0] * a_b[0 * 4 + j] + a_a[i * 4 + 1] * a_b[1 * 4 + j] + a_a[i * 4 + 2] * a_b[2 * 4 + j] + (j == 3 ? a_a[i * 4 + 3] : 0.0f);
				}
			}
			std::memcpy(a_out, r, sizeof(r));
		}

		// The inverse of an anchor matrix (its 3x3 part is orthogonal: a turn about Y, maybe mirrored in x).
		void InverseRigid(const float* a_m, float* a_out)
		{
			const float r[12] = { a_m[0], a_m[4], a_m[8], 0, a_m[1], a_m[5], a_m[9], 0, a_m[2], a_m[6], a_m[10], 0 };
			std::memcpy(a_out, r, sizeof(r));
			for (int i = 0; i < 3; ++i) {
				a_out[i * 4 + 3] = -(r[i * 4 + 0] * a_m[3] + r[i * 4 + 1] * a_m[7] + r[i * 4 + 2] * a_m[11]);
			}
		}

		// The camera sees p (its frustum with a metre's margin for the dog's body; not occlusion). Unknown camera: yes.
		bool InView(const RenderCamera& a_c, const Vec3& a_p)
		{
			if (!a_c.valid) {
				return true;
			}
			const Vec3  d = a_p - a_c.pos;
			const float z = Dot(d, Normalize(a_c.fwd));
			if (z < 0.2f || z > 250.0f) {
				return false;
			}
			const float ty = std::tan(a_c.fovY * 0.5f) * 1.15f;
			const float tx = ty * (a_c.aspect > 0.1f ? a_c.aspect : 16.0f / 9.0f);
			return std::fabs(Dot(d, Normalize(a_c.right))) <= tx * z + 1.0f && std::fabs(Dot(d, Normalize(a_c.up))) <= ty * z + 1.0f;
		}

		// The buddy walks on MGSV's flat sandbox: lift (or lower) its draws onto Elden Ring's ground under it (er/Buddy.cpp's
		// ray), smoothed so a rock or a step doesn't make it pop; not while Snake rides it (the puppet carries both).
		// (0.8.0 polish) Also: drawn through Snake's own anchor while his body is turned for a staged move (Puppet::StagedDrawAnchor turned
		// the buddy around him with it); a waiting D-Dog's Wait correction (Buddy::WaitShift, unsmoothed); D-Dog's draws guarded against
		// one-frame jumps and snaps (StepGuard: what the player sees never moves more than 0.5 m a frame but out of view).
		void GroundBuddy(RenderSnapshot& a_s, const Frame& a_f)
		{
			const float a_dt = a_f.dt;
			const bool placed = !a_s.riding && Buddy::HasEstimate();
			// The ground where the body is drawn: measured at the unguarded place (Buddy::DrawGroundOffset; the placement's own guard may
			// still hold the move this guard took out of view), kept as it was while this guard holds a jump (the body stays where it
			// was) -- review 0.8.0.
			if (!(g_drawGuard.Holding() && a_f.guest.buddyType == Buddy::kDog)) {
				g_groundTarget = placed ? std::clamp(Buddy::DrawGroundOffset(), -1.5f, 1.5f) : 0.0f;
				g_groundShiftTarget = placed ? Buddy::DrawGroundShift() : Vec3{};
			}
			const float target = placed ? g_groundTarget : 0.0f;
			const float k = std::min(1.0f, std::max(0.0f, a_dt) * 8.0f);
			g_buddyLift += (target - g_buddyLift) * k;
			// The floor-near-Snake rule's horizontal move (er/Buddy.h DrawShift), smoothed the same way, at most 30 m.
			Vec3 shift = placed ? g_groundShiftTarget : Vec3{};
			if (!Finite(shift) || LengthXZ(shift) > 30.0f) {
				shift = {};
			}
			g_buddyShift = g_buddyShift + (shift - g_buddyShift) * k;
			Vec3 wait = placed ? Buddy::WaitShift() : Vec3{};
			if (!Finite(wait) || LengthXZ(wait) > 40.0f) {
				wait = {};
			}
			const float* am = a_s.anchor;
			bool         any = false;
			for (const auto& d : a_s.draws) {
				any = any || d.owner == proto::kOwnerBuddy;
			}
			if (!any) {
				return;
			}
			// Staged: back to Snake's own anchor for the buddy (C = A_staged^-1 * A_own, applied to its matrices).
			float effA[12];  // the anchor the buddy's draws end up drawn with
			std::memcpy(effA, am, sizeof(effA));
			if (!a_s.riding) {
				float own[12];
				AnchorMatrix(Puppet::CurrentAnchor(), own);
				bool differs = false;
				for (int i = 0; i < 12; ++i) {
					differs = differs || std::fabs(own[i] - am[i]) > 1e-4f;
				}
				if (differs) {
					std::memcpy(effA, own, sizeof(effA));
					float inv[12], c[12];
					InverseRigid(am, inv);
					Compose(inv, own, c);
					std::vector<std::uint8_t> done(a_s.matrices.size() / 12, 0);
					for (const auto& d : a_s.draws) {
						if (d.owner != proto::kOwnerBuddy) {
							continue;
						}
						for (std::uint32_t m = d.firstMatrix; m < d.firstMatrix + d.matrixCount && m < done.size(); ++m) {
							if (!done[m]) {
								done[m] = 1;
								Compose(c, &a_s.matrices[std::size_t(m) * 12], &a_s.matrices[std::size_t(m) * 12]);
							}
						}
					}
					std::scoped_lock lock(g_guardStatsLock);
					++g_guardStats.staged;
				}
			}
			if (const int want = g_traceWant.exchange(0)) {
				if (want > 0 && !g_trace) {
					const auto path = ModuleDir() + L"PhantomRingBuddyTrace.csv";
					g_trace = _wfopen(path.c_str(), L"ab");
					g_traceRows = 0;
					if (g_trace) {
						std::fputs("# ms,guestFrame,odoEpoch,raw.x,raw.y,raw.z,drawn.x,drawn.y,drawn.z,act,rawStep,drawnStep,jump,rawIn,drawnIn,gflags,bflags,pe.x,pe.y,"
						           "pe.z,wait.x,wait.z,lift,dt,fromPub,est.x,est.y,est.z\n",
							g_trace);
					}
					Log::Info("render: D-Dog's draw trace {}", g_trace ? "on (PhantomRingBuddyTrace.csv next to the DLL, one line per frame it is drawn)" : "could NOT be opened");
				} else if (want < 0 && g_trace) {
					std::fclose(g_trace);
					g_trace = nullptr;
					Log::Info("render: D-Dog's draw trace off ({} lines)", g_traceRows);
				}
			}
			// D-Dog's drawn place: its published place (below), else the median of its draws' own centres (a stray draw doesn't move it).
			Vec3       guardOffset{};
			const auto now = ::GetTickCount64();
			const bool dog = a_f.guest.buddyType == Buddy::kDog && Cfg().dog.enabled && Cfg().dog.stepGuard && !a_s.riding;
			const Vec3 pe = Puppet::CurrentAnchor().pe;
			const auto nonce = Link::Get().Nonce();
			const auto epoch = Puppet::StartEpoch();
			if (dog && g_guardPeValid && !(LengthXZ(pe - g_guardPe) <= 5.0f)) {
				// Elden Ring moved its world origin (8 m steps): the guard's places move with it, nothing jumps.
				if (const auto shift = WorldShift::ShiftOf(g_guardPe, pe)) {
					g_drawGuard.Shift(*shift);
					g_guardPe = g_guardPe + *shift;
				}
			}
			if (!dog || nonce != g_guardNonce || now - g_guardDrawsAt > 250 || !g_guardPeValid || !(LengthXZ(pe - g_guardPe) <= 5.0f) || Puppet::ClimbVisual() ||
				epoch != g_guardEpoch) {
				if (dog && g_guardNonce && g_guardDrawsAt) {
					std::scoped_lock lock(g_guardStatsLock);
					++g_guardStats.resets;
				}
				g_drawGuard.Reset();
				g_guardNonce = nonce;
			}
			g_guardPe = pe;
			g_guardEpoch = epoch;
			g_guardPeValid = Finite(pe);
			g_guardDrawsAt = now;
			if (dog) {
				std::vector<float> xs, ys, zs;
				for (const auto& d : a_s.draws) {
					if (d.owner != proto::kOwnerBuddy || d.matrixCount == 0 || std::size_t(d.firstMatrix + d.matrixCount) * 12 > a_s.matrices.size()) {
						continue;
					}
					Vec3          sum{};
					std::uint32_t n = 0;
					for (std::uint32_t m = d.firstMatrix; m < d.firstMatrix + d.matrixCount && n < 64; ++m, ++n) {
						const float* t = &a_s.matrices[std::size_t(m) * 12];
						sum += Vec3{ t[3], t[7], t[11] };
					}
					sum = sum * (1.0f / static_cast<float>(n));
					xs.push_back(sum.x);
					ys.push_back(sum.y);
					zs.push_back(sum.z);
				}
				if (!xs.empty()) {
					const auto median = [](std::vector<float>& a_v) {
						std::nth_element(a_v.begin(), a_v.begin() + a_v.size() / 2, a_v.end());
						return a_v[a_v.size() / 2];
					};
					const Vec3 est{ median(xs), median(ys), median(zs) };
					// What the guard watches: the dog's published place (GuestState buddyPos relative to Snake's root -- the guest moves its
					// draws to it, at most 14 m/s), through the anchor its draws end up with. The integration run (2026-11-04) found the
					// median of its draws' matrix means (skinning matrices: bind-pose offsets that swing as the dog turns) jumping 1.5-6 m
					// while the published place moved under 0.25 m: holding such a "jump" moved the real body by it (~190 offset steps over
					// 0.5 m in 20 minutes, all in view). The estimate stays the fallback while the guest has no place for it.
					const auto& gs = a_f.guest;
					const bool  pub = (gs.buddyFlags & proto::kBuddyPresent) && (gs.buddyFlags & proto::kBuddyPosValid);
					const Vec3  relPub{ gs.buddyPos[0] - gs.rootPos[0], gs.buddyPos[1] - gs.rootPos[1], gs.buddyPos[2] - gs.rootPos[2] };
					const bool  usePub = pub && Finite(relPub) && LengthXZ(relPub) < 200.0f;
					const Vec3  rel = usePub ? relPub : est;
					const Vec3  raw = Apply(usePub ? effA : am, rel) + Vec3{ 0, g_buddyLift, 0 } + g_buddyShift + wait;
					const Vec3  estEr = Apply(am, est);
					if (usePub != g_guardFromPub) {
						g_guardFromPub = usePub;
						g_drawGuard.Reset();  // another point: nothing to compare it with
					}
					const bool rawIn = InView(a_s.camera, raw + Vec3{ 0, 0.5f, 0 });
					const bool drawnIn = InView(a_s.camera, g_drawGuard.Drawn() + Vec3{ 0, 0.5f, 0 });
					const auto st = g_drawGuard.Update(raw, rawIn, drawnIn, a_dt, now, buddyguard::Rules{});
					guardOffset = st.offset;
					const bool taken = st.act == buddyguard::Act::kTakenOut;
					{
						std::scoped_lock lock(g_guardStatsLock);
						auto& gs = g_guardStats;
						++gs.acts[static_cast<std::size_t>(st.act)];
						++gs.frames;
						const float scale = std::max(1.0f, a_dt * 60.0f);  // per 60 Hz frame
						gs.rawOver += st.raw > 0.5f * scale ? 1 : 0;
						gs.rawMax = std::max(gs.rawMax, st.raw / scale);
						if (!taken) {
							gs.drawnOver += st.drawn > 0.5f * scale + 1e-3f ? 1 : 0;
							gs.drawnMax = std::max(gs.drawnMax, st.drawn / scale);
						}
						gs.jumpMax = std::max(gs.jumpMax, st.jump);
						gs.heldMaxMs = std::max(gs.heldMaxMs, st.heldMs);
					}
					if (g_trace) {
						const Vec3 drawn = raw + st.offset;
						std::fprintf(g_trace, "%llu,%llu,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%d,%d,%#x,%#x,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f\n",
							static_cast<unsigned long long>(now), static_cast<unsigned long long>(a_s.guestFrame), static_cast<unsigned>(a_f.guest.odoEpoch), raw.x, raw.y,
							raw.z, drawn.x, drawn.y, drawn.z, static_cast<int>(st.act), st.raw, st.drawn, st.jump, rawIn ? 1 : 0, drawnIn ? 1 : 0, a_f.guest.flags,
							a_f.guest.buddyFlags, pe.x, pe.y, pe.z, wait.x, wait.z, g_buddyLift, a_dt, usePub ? 1 : 0, estEr.x, estEr.y, estEr.z);
						if (++g_traceRows % 120 == 0) {
							std::fflush(g_trace);
						}
					}
					if (st.act != buddyguard::Act::kNone && st.act != buddyguard::Act::kHolding && st.act != buddyguard::Act::kLimited &&
						(st.act != buddyguard::Act::kSnap || now - g_guardLogAt >= 250)) {
						g_guardLogAt = now;
						Log::Info("render: guard (D-Dog as drawn): {} -- {:.2f} m{} (raw step {:.2f} m, drawn step {:.2f} m; raw {} / drawn {} in view)",
							buddyguard::Name(st.act), st.jump, st.heldMs ? std::format(" after {} ms", st.heldMs) : std::string(), st.raw, st.drawn,
							rawIn ? "is" : "not", drawnIn ? "is" : "not");
					}
				}
			}
			// ER-space shift -> the draws' anchor-relative MGSV space: the anchor matrix's x and z columns are orthonormal (y is up).
			const Vec3   total = g_buddyShift + wait + guardOffset;
			const float  sx = total.x * am[0] + total.y * am[4] + total.z * am[8];
			const float  sz = total.x * am[2] + total.y * am[6] + total.z * am[10];
			const float  lift = g_buddyLift + guardOffset.y;
			const bool   moveXZ = std::fabs(sx) + std::fabs(sz) >= 0.005f;
			if (std::fabs(lift) < 0.005f && !moveXZ) {
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
						a_s.matrices[std::size_t(m) * 12 + 7] += lift;  // y: the anchor turns about y only
						if (moveXZ) {
							a_s.matrices[std::size_t(m) * 12 + 3] += sx;
							a_s.matrices[std::size_t(m) * 12 + 11] += sz;
						}
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
			if (Pacing::Draws(a_f, paced, a_s.matrices, a_s.guestFrame, &a_s.guestYaw)) {
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
				a_s.guestYaw = list->anchorYaw;
				recs = reinterpret_cast<const proto::DrawRecord*>(base + sizeof(proto::DrawList));
				const auto* mats = reinterpret_cast<const float*>(base + sizeof(proto::DrawList) + list->drawCount * sizeof(proto::DrawRecord));
				a_s.matrices.assign(mats, mats + std::size_t(list->matrixCount) * 12);
				drawCount = list->drawCount;
				matrixCount = list->matrixCount;
			}
			const auto& rc = Cfg().render;
			// Dismissed in MGSV: what it still draws (leaving) stays out. On a ladder (Snake drawn where the Tarnished climbs) the
			// buddy would hang beside him in the air: out too, back with the puppet.
			const bool  climbing = Puppet::ClimbVisual();
			const bool  buddyOut = rc.drawBuddy && !Buddy::Absent() && !climbing;
			a_s.draws.reserve(drawCount);
			for (std::uint32_t i = 0; i < drawCount; ++i) {
				const auto& d = recs[i];
				if (d.matrixCount == 0 || std::uint64_t(d.firstMatrix) + d.matrixCount > matrixCount) {
					++g_badLists;
					continue;
				}
				if ((d.owner == proto::kOwnerPlayer && !rc.drawPlayer) || (d.owner == proto::kOwnerBuddy && !buddyOut) ||
					(d.owner >= proto::kOwnerVehicle && d.owner != proto::kOwnerProjectile && (!rc.drawOther || climbing))) {  // projectiles: Projectiles::Apply
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
		// On a ladder ([Ladder] snake) the puppet is off while Elden Ring climbs with the hidden Tarnished: Snake is drawn at the
		// anchor the host keeps on him (Puppet::ClimbVisual).
		const bool camera = ReadCamera(s->camera);
		if (dying >= 0.0f) {
			// Why Snake is (not) drawn while he dies (Puppet's death trace, the log): seen once in 0.6.0 testing he wasn't.
			const char* why = !rc.enabled ? "rendering off" : s->dissolve >= 1.0f ? "dissolved (death_visual)" : !camera ? "no camera" :
			                  !a_f.guestReady                                ? "MGSV not ready" :
			                  a_f.loading                                    ? "loading" :
			                                                                   nullptr;
			g_dyingWhy.store(why, std::memory_order_relaxed);
		}
		if (rc.enabled && s->dissolve < 1.0f && camera && (Puppet::Active() || dying >= 0.0f || Puppet::ClimbVisual()) && a_f.guestReady && !a_f.loading) {
			const Anchor& a = Puppet::CurrentAnchor();
			AnchorMatrix(a, s->anchor);
			s->mirrored = a.axes.mirrorX < 0;
			s->riding = (a_f.guest.flags & proto::kGuestOnVehicle) != 0;
			s->firstPerson = (a_f.guest.flags & proto::kGuestFirstPerson) != 0;
			s->draw = CopyDrawList(*s, a_f);
			// MGSV plays a staged move (a climb-up / vault at the airport's obstacle, or he is still on its ladder as the puppet
			// starts): his body there faces that obstacle, turned to face Elden Ring's move instead (er/StagedFacing.h). Before the
			// projectiles: they are placed through this same anchor.
			if (s->draw && std::isfinite(s->guestYaw)) {
				if (const auto staged = Puppet::StagedDrawAnchor(s->guestYaw)) {
					AnchorMatrix(*staged, s->anchor);
				}
			}
			// MGSV's projectile draws out, the flown grenades / rockets in (roadmap 5.4) -- also without a list from MGSV this
			// frame: Throw / Rounds leave their ball out for a copy placed here, which must not vanish while MGSV stalls.
			s->draw = Projectiles::Apply(*s);
			if (s->draw) {
				GroundBuddy(*s, a_f);
			}
			if (dying >= 0.0f && !s->draw) {
				g_dyingWhy.store("no draw list from MGSV", std::memory_order_relaxed);
			}
		}
		if (dying >= 0.0f) {
			g_dyingDraws.store(s->draw ? static_cast<std::uint32_t>(s->draws.size()) : 0u, std::memory_order_relaxed);
		}
		// The host's own world markers (er/RenderMarkers.h); their owners show none in menus, cutscenes, loads.
		s->markers = RenderMarkers::Collect();
		g_hasMarkers.store(!s->markers.empty(), std::memory_order_relaxed);
		// MGSV's night-vision goggles (protocol v10 kGuestNvg): on Elden Ring's frame while Snake plays with them on, faded in
		// and out like the goggles; not in cutscenes, loads or death.
		{
			const bool  want = rc.enabled && rc.nvg && (Puppet::Active() || Puppet::ClimbVisual()) && a_f.guestReady && (a_f.guest.flags & proto::kGuestNvg) && !a_f.cutscene &&
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
			if (!Puppet::Active() && !Puppet::ClimbVisual() && dying < 0.0f && a_f.inGame && !a_f.loading && a_f.player) {
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

	std::string BuddyGuardDescribe()
	{
		std::scoped_lock lock(g_guardStatsLock);
		const auto&      gs = g_guardStats;
		const auto       a = [&](buddyguard::Act x) { return gs.acts[static_cast<std::size_t>(x)]; };
		return std::format("D-Dog as drawn ([Dog] step_guard {}): {} frames measured -- steps over 0.5 m: {} raw, {} drawn (out-of-view takes left out); largest "
		                   "step raw {:.2f} m, drawn {:.2f} m (per 60 Hz frame); {} snaps spread, {} jumps held ({} came back, {} eased in, {} taken out of view, {} "
		                   "taken in view late), longest hold {} ms, largest jump {:.2f} m; {} frames limited; {} starts over; {} frames drawn through Snake's own "
		                   "anchor (a staged move)",
			Cfg().dog.stepGuard ? "on" : "off", gs.frames, gs.rawOver, gs.drawnOver, gs.rawMax, gs.drawnMax, a(buddyguard::Act::kSnap), a(buddyguard::Act::kHold),
			a(buddyguard::Act::kReturned), a(buddyguard::Act::kEase), a(buddyguard::Act::kTakenOut), a(buddyguard::Act::kTakenLate), gs.heldMaxMs, gs.jumpMax,
			a(buddyguard::Act::kLimited), gs.resets, gs.staged);
	}

	void BuddyTrace(bool a_on) { g_traceWant.store(a_on ? 1 : -1); }

	void BuddyGuardReset()
	{
		std::scoped_lock lock(g_guardStatsLock);
		g_guardStats = {};
	}

	std::string DyingDraw()
	{
		const char* why = g_dyingWhy.load(std::memory_order_relaxed);
		const auto  n = g_dyingDraws.load(std::memory_order_relaxed);
		return n ? std::format("Snake drawn ({} draws)", n) : std::format("Snake NOT drawn ({})", why ? why : "nothing to draw");
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
