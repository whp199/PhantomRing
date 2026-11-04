#include "Buddy.h"

#include "BuddyGuard.h"
#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Hud.h"
#include "Link.h"
#include "Log.h"
#include "Pacing.h"
#include "Puppet.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <format>

namespace phantom::er::Buddy
{
	namespace
	{
		std::uint16_t g_type{ 0xFFFF };
		bool          g_estimate{ false };
		Vec3          g_pos;
		float         g_groundOffset{ 0 };
		Vec3          g_drawShift;           // the floor-near-Snake rule's horizontal move of the drawn buddy (ER space)
		float         g_drawGroundOffset{ 0 };  // ...both at the unguarded place (D-Dog's draws: RenderScene's own guard decides where)
		Vec3          g_drawGroundShift;
		std::uint64_t g_drawGroundApart{ 0 };   // frames the two were measured apart
		std::uint64_t g_floorShifts{ 0 }, g_floorSnake{ 0 }, g_floorNone{ 0 };  // placements by that rule: moved / Snake's level / none
		std::uint32_t g_draws{ 0 };
		std::uint32_t g_flags{ 0 };
		float         g_heading{ 0 };
		const char*   g_source{ "none" };
		bool          g_loggedFirst{ false };
		std::atomic<bool> g_absent{ false };
		bool          g_seenOut{ false };      // this guest has had a buddy out (Present / Loading) since it attached
		std::uint64_t g_nonce{ 0 };            // the guest session those belong to
		std::uint16_t g_typeShown{ 0xFFFF };   // what the last corner message was about
		bool          g_outShown{ false };
		std::uint64_t g_changeFrames{ 0 };     // frames the new state has held (a change shows once it held 10 frames)
		std::uint16_t g_pendingType{ 0xFFFF };
		bool          g_pendingOut{ false };

		// (0.8.0 polish) D-Dog's placement guarded against one-frame jumps (er/BuddyGuard.h StepGuard, the placement's rules) and a
		// waiting D-Dog pinned to its Elden Ring spot (WaitPin: Snake's slip at Elden Ring's walls since the Wait).
		buddyguard::StepGuard g_placeGuard;
		buddyguard::WaitPin   g_pin;
		Vec3                  g_pinShift;
		Vec3                  g_lastMapped;        // D-Dog's place as mapped last frame (no correction): where a Wait starts from
		bool                  g_haveMapped{ false };
		bool                  g_slipHave{ false };
		Vec3                  g_prevPe;
		double                g_prevOdo[3]{};
		std::uint32_t         g_prevEpoch{ 0 };
		std::uint64_t         g_pinWaits{ 0 }, g_pinEnds{ 0 }, g_pinResets{ 0 };
		float                 g_pinMax{ 0 };      // m: the largest correction a wait needed
		std::array<std::uint64_t, static_cast<std::size_t>(buddyguard::Act::kCount)> g_placeActs{};
		float                 g_placeRawMax{ 0 }, g_placeOutMax{ 0 };  // m in a frame: the raw placement's largest step, the guarded one's
		ULONGLONG             g_placeLogAt{ 0 };

		void ResetGuards()
		{
			g_placeGuard.Reset();
			g_slipHave = false;
		}

		// Snake's slip this frame: his Elden Ring step minus his MGSV step (the odometer, as the puppet consumes it) mapped to Elden Ring
		// -- not zero where Elden Ring's walls stop him (MGSV's Snake goes on). Horizontal; nullopt when there is no baseline (a new
		// odometer, the puppet just started) or Snake was put somewhere new (a grace, a load: over 5 m in a frame).
		std::optional<Vec3> Slip(const Frame& a_f, const Anchor& a_a)
		{
			const auto& g = a_f.guest;
			const auto& m = Pacing::Current();
			const bool  paced = m.valid && m.epoch == g.odoEpoch;
			const double odo[3]{ paced ? m.odo[0] : g.odo[0], paced ? m.odo[1] : g.odo[1], paced ? m.odo[2] : g.odo[2] };
			const bool   had = g_slipHave && g_prevEpoch == g.odoEpoch;
			const Vec3   dPe = a_a.pe - g_prevPe;
			const Vec3   dOdo{ static_cast<float>(odo[0] - g_prevOdo[0]), static_cast<float>(odo[1] - g_prevOdo[1]), static_cast<float>(odo[2] - g_prevOdo[2]) };
			g_slipHave = true;
			g_prevEpoch = g.odoEpoch;
			g_prevPe = a_a.pe;
			std::copy(std::begin(odo), std::end(odo), g_prevOdo);
			if (!had || !Finite(dPe) || !Finite(dOdo) || LengthXZ(dPe) > 5.0f || LengthXZ(dOdo) > 5.0f) {
				return std::nullopt;
			}
			Vec3 slip = dPe - a_a.DirToEr(dOdo);
			slip.y = 0;
			return slip;
		}

		// Mean translation of the buddy's draw matrices (3x4 row-major, translation in [3], [7],
		// [11], relative to the list's anchor). Skinning matrices carry bind-pose offsets, so this
		// is a centroid estimate (good to a meter or so), not a bone position.
		bool EstimateFromDraws(Vec3& a_mgsvWorld, std::uint32_t& a_draws)
		{
			std::uint32_t bytes = 0;
			const auto*   list = Link::Get().FrontDraw();
			if (!list || !Link::Get().FrontDrawBytes(bytes)) {
				return false;
			}
			const auto* records = reinterpret_cast<const proto::DrawRecord*>(list + 1);
			const auto* matrices = reinterpret_cast<const float*>(records + list->drawCount);
			Vec3        sum{};
			std::uint32_t n = 0;
			a_draws = 0;
			for (std::uint32_t i = 0; i < list->drawCount; ++i) {
				const auto& r = records[i];
				if (r.owner != proto::kOwnerBuddy || r.matrixCount == 0 || r.firstMatrix + r.matrixCount > list->matrixCount) {
					continue;
				}
				++a_draws;
				for (std::uint32_t m = 0; m < r.matrixCount && m < 64; ++m) {
					const float* t = matrices + 12 * (r.firstMatrix + m);
					sum += Vec3{ t[3], t[7], t[11] };
					++n;
				}
			}
			if (!n) {
				return false;
			}
			a_mgsvWorld = sum * (1.0f / static_cast<float>(n)) + Vec3{ list->anchorPos[0], list->anchorPos[1], list->anchorPos[2] };
			return Finite(a_mgsvWorld);
		}
	}

	const char* TypeName(std::uint16_t a_type)
	{
		switch (a_type) {
		case kNone:
			return "none";
		case kHorse:
			return "D-Horse";
		case kDog:
			return "D-Dog";
		case kQuiet:
			return "Quiet";
		case kWalkerGear:
			return "D-Walker";
		default:
			return "unknown";
		}
	}

	bool Absent() { return g_absent.load(std::memory_order_relaxed); }

	std::string ChangeText(std::uint16_t a_typeBefore, bool a_outBefore, std::uint16_t a_typeAfter, bool a_outAfter)
	{
		const bool before = a_outBefore && a_typeBefore != kNone;
		const bool after = a_outAfter && a_typeAfter != kNone;
		if (before && !after) {
			return std::format("{} dismissed -- no buddy now", TypeName(a_typeBefore));
		}
		if (!after) {
			return "";
		}
		if (!before) {
			return std::format("Buddy: {} is out", TypeName(a_typeAfter));
		}
		if (a_typeBefore != a_typeAfter) {
			return std::format("Buddy: {} instead of {}", TypeName(a_typeAfter), TypeName(a_typeBefore));
		}
		return "";
	}

	void Update(const Frame& a_f)
	{
		g_estimate = false;
		if (!Cfg().buddy || !a_f.guestReady) {
			return;
		}
		const auto& g = a_f.guest;
		// A buddy out or not, and changes of it in the corner (MGSV's own buddy menu: iDroid > Buddy Support, or `!buddy`).
		{
			const std::uint64_t nonce = Link::Get().Nonce() ^ (std::uint64_t(g.odoEpoch) << 32);
			if (nonce != g_nonce) {
				g_nonce = nonce;
				g_seenOut = false;
				g_typeShown = 0xFFFF;
				g_changeFrames = 0;
			}
			const bool out = g.buddyType != kNone && (g.buddyFlags & (proto::kBuddyPresent | proto::kBuddyLoading));
			g_seenOut = g_seenOut || out;
			const bool absent = g.buddyType == kNone || (g_seenOut && !out);
			if (absent != g_absent.load(std::memory_order_relaxed)) {
				g_absent = absent;
				Log::Info("buddy: {}", absent ? std::format("none out (type {}, flags {:#x}): its draws are left out, it is not placed", TypeName(g.buddyType), g.buddyFlags)
				                              : std::format("{} out again: drawn and placed", TypeName(g.buddyType)));
			}
			if (g_typeShown == 0xFFFF) {
				g_typeShown = g.buddyType;  // the first state after attaching: MGSV's corner text already said what is out
				g_outShown = out;
			} else if (g.buddyType != g_typeShown || out != g_outShown) {
				if (g.buddyType != g_pendingType || out != g_pendingOut) {
					g_pendingType = g.buddyType;
					g_pendingOut = out;
					g_changeFrames = 0;
				}
				if (++g_changeFrames >= 10) {
					if (const auto text = ChangeText(g_typeShown, g_outShown, g.buddyType, out); !text.empty()) {
						Hud::Notify(text, Hud::Tone::kInfo);
						Log::Info("buddy: {} (type {} -> {}, out {} -> {})", text, g_typeShown, g.buddyType, g_outShown, out);
					}
					g_typeShown = g.buddyType;
					g_outShown = out;
				}
			}
		}
		const auto before = g_type;
		if (g.buddyType != g_type) {
			g_type = g.buddyType;
			Log::Info("buddy: MGSV buddy is {} ({})", TypeName(g_type), g_type);
		}
		if (g.buddyFlags != g_flags) {
			const auto rising = g.buddyFlags & ~g_flags;
			if (rising & proto::kBuddyMissing) {
				Log::Warn("buddy: {} is not available in this MGSV save (not developed, or lost)", TypeName(g_type));
			}
			if (rising & proto::kBuddyPresent) {
				Log::Info("buddy: {} deployed (object id {:#x})", TypeName(g_type), g.buddyObjectId);
			}
			if ((g_flags & proto::kBuddyPresent) && !(g.buddyFlags & proto::kBuddyPresent)) {
				Log::Info("buddy: {} gone", TypeName(before == 0xFFFF || before == kNone ? g_type : before));
			}
			if ((rising | (g_flags & ~g.buddyFlags)) & proto::kBuddyRidden) {
				Log::Info("buddy: Snake {} {}", (g.buddyFlags & proto::kBuddyRidden) ? "rides" : "got off", TypeName(g_type));
			}
			g_flags = g.buddyFlags;
		}
		if (!Puppet::Active() || g_absent.load(std::memory_order_relaxed)) {
			ResetGuards();
			return;
		}
		// Source: the guest's own buddy pose (protocol v2) when it has one this frame, else the
		// centroid of the buddy's captured draws (phase 2+).
		Vec3 world;
		if ((g.buddyFlags & proto::kBuddyPresent) && (g.buddyFlags & proto::kBuddyPosValid)) {
			world = { g.buddyPos[0], g.buddyPos[1], g.buddyPos[2] };
			g_source = "guest pose";
			g_draws = 0;
		} else if (EstimateFromDraws(world, g_draws)) {
			g_source = "draw centroid";
		} else {
			return;
		}
		if (!Finite(world)) {
			return;
		}
		const auto& anchor = Puppet::CurrentAnchor();
		Vec3        er = anchor.PointToEr(world);
		Vec3        hit;
		const bool  dog = g.buddyType == kDog && !(g.buddyFlags & proto::kBuddyRidden) && !Puppet::Riding();
		const auto  slip = Slip(a_f, anchor);
		// (0.8.0 polish) A waiting D-Dog stays on its Elden Ring spot: Snake's slip at Elden Ring's walls since the Wait is added back
		// (er/BuddyGuard.h WaitPin; the guest keeps its MGSV place through the treadmill's jumps).
		if (dog && Cfg().dog.enabled && Cfg().dog.waitPin) {
			if (!slip && (g_pin.Waiting() || LengthXZ(g_pin.Shift()) > 0.0f)) {
				++g_pinResets;
				Log::Info("buddy: D-Dog's Wait correction ({:.2f} m) dropped -- Snake was put somewhere new (or the odometer restarted)", LengthXZ(g_pin.Shift()));
				g_pin.Reset();
			}
			const bool waited = g_pin.Waiting();
			g_lastMapped = er;
			g_haveMapped = true;
			g_pinShift = g_pin.Update(slip.value_or(Vec3{}), er, (g.flags & (proto::kGuestClimbing | proto::kGuestMantling)) != 0, a_f.dt);
			g_pinMax = std::max(g_pinMax, LengthXZ(g_pinShift));
			if (waited && !g_pin.Waiting()) {
				++g_pinEnds;
				Log::Info("buddy: D-Dog's wait is over ({}; it moved {:.2f} m that Snake's slip doesn't explain) -- its Elden Ring correction {:.2f} m eases out",
					g_pin.EndedBy() ? g_pin.EndedBy() : "?", g_pin.Moved(), LengthXZ(g_pinShift));
			}
			er = er + g_pinShift;
		} else {
			g_pin.Reset();
			g_pinShift = {};
			g_haveMapped = false;
		}
		// (0.8.0 polish) D-Dog's placement never follows a one-frame jump (er/BuddyGuard.h StepGuard): what the pick, the spotting and
		// the ground under its drawn body use.
		const Vec3 unguarded = er;  // after the pin, before the guard: where the drawn body may already be (RenderScene's own guard)
		if (dog && Cfg().dog.enabled && Cfg().dog.stepGuard) {
			static std::uint32_t epoch = 0;
			if (!slip || Puppet::StartEpoch() != epoch) {
				// Snake put somewhere new (over 5 m in a frame: a grace, a load) or the puppet starting: nothing to hold against. A
				// 1.5-5 m re-anchor (a shove) is held / eased like any jump (review 0.8.0).
				g_placeGuard.Reset();
			}
			epoch = Puppet::StartEpoch();
			const auto now = ::GetTickCount64();
			const auto st = g_placeGuard.Update(er, true, true, a_f.dt, now, buddyguard::PlacementRules());
			++g_placeActs[static_cast<std::size_t>(st.act)];
			g_placeRawMax = std::max(g_placeRawMax, st.raw);
			g_placeOutMax = std::max(g_placeOutMax, st.drawn);
			if (st.act != buddyguard::Act::kNone && st.act != buddyguard::Act::kHolding && st.act != buddyguard::Act::kLimited && (st.act != buddyguard::Act::kSnap || now - g_placeLogAt >= 250)) {
				g_placeLogAt = now;
				Log::Info("buddy: guard (D-Dog's place): {} -- {:.2f} m{} (raw step {:.2f} m, placed step {:.2f} m)", buddyguard::Name(st.act), st.jump,
					st.heldMs ? std::format(" after {} ms", st.heldMs) : std::string(), st.raw, st.drawn);
			}
			er = er + st.offset;
		} else {
			g_placeGuard.Reset();
		}
		// He walks on MGSV's flat sandbox: snap his feet to Elden Ring's ground under him.
		struct Ground
		{
			float offset{ 0 };
			Vec3  shift;
			int   kind{ 0 };  // 0 the ground under him, 1 moved toward Snake, 2 Snake's level, 3 none (floating)
		};
		const bool canFloor = !Puppet::Riding() && !(g.buddyFlags & proto::kBuddyRidden);
		const auto groundAt = [&](const Vec3& a_p) {
			Ground r;
			Vec3   h;
			if (game::CastRay(a_p + Vec3{ 0, 2.5f, 0 }, Vec3{ 0, -8.0f, 0 }, h, a_f.player.chr, Cfg().rayFilter)) {
				r.offset = h.y - a_p.y;
			} else if (canFloor) {
				// No ground within 8 m under him (beside a tower top, a cliff's edge after a ladder): the floor at Snake's height under
				// him, else the nearest one on the line toward Snake (docs/research/ddog.md §3.7) -- he was drawn floating in the air.
				const Vec3 snake = anchor.pe;
				const auto floorAt = [&](const Vec3& a_q) -> std::optional<float> {
					Vec3 fh;
					if (game::CastRay(Vec3{ a_q.x, snake.y + 1.5f, a_q.z }, Vec3{ 0, -3.0f, 0 }, fh, a_f.player.chr, Cfg().rayFilter)) {
						return fh.y;
					}
					return std::nullopt;
				};
				if (const auto floor = FloorNearSnake(a_p, snake, floorAt)) {
					r.offset = floor->y - a_p.y;
					r.shift = floor->shift;
					r.kind = LengthXZ(floor->shift) > 1e-3f ? 1 : 2;
				} else {
					r.kind = 3;
				}
			} else {
				r.kind = 3;
			}
			return r;
		};
		const Ground gr = groundAt(er);
		const Vec3   guarded = er;
		g_groundOffset = gr.offset;
		g_drawShift = gr.shift;
		er = Vec3{ guarded.x + gr.shift.x, guarded.y + gr.offset, guarded.z + gr.shift.z };
		g_floorShifts += gr.kind == 1 ? 1 : 0;
		g_floorSnake += gr.kind == 2 ? 1 : 0;
		g_floorNone += gr.kind == 3 && canFloor ? 1 : 0;
		// The drawn body's ground: where the guard holds the placement but the renderer's own guard may already draw the body at the
		// new place (a leash return out of view), the lift and floor shift are measured there -- they were the old place's for up to
		// 500 ms (review 0.8.0). One more ray only while the two differ.
		if (LengthXZ(unguarded - guarded) > 0.3f) {
			const Ground dg = groundAt(unguarded);
			g_drawGroundOffset = dg.offset;
			g_drawGroundShift = dg.shift;
			++g_drawGroundApart;
		} else {
			g_drawGroundOffset = g_groundOffset;
			g_drawGroundShift = g_drawShift;
		}
		g_pos = er;
		g_heading = MgsvHeading(g.buddyYaw, anchor.axes) + anchor.delta;
		g_estimate = true;
		if (!g_loggedFirst) {
			g_loggedFirst = true;
			Log::Info("buddy: first placement from {}: ER ({:.2f}, {:.2f}, {:.2f}), ground offset {:.2f} m", g_source, er.x, er.y, er.z, g_groundOffset);
		}
	}

	void Draw()
	{
		if (!g_estimate) {
			return;
		}
		game::SetDrawColor(0.1f, 0.9f, 1.0f, 1.0f);
		game::DrawLine(g_pos, g_pos + Vec3{ 0, 2.2f, 0 });
		game::DrawLine(g_pos + Vec3{ -0.5f, 0.05f, 0 }, g_pos + Vec3{ 0.5f, 0.05f, 0 });
		game::DrawLine(g_pos + Vec3{ 0, 0.05f, -0.5f }, g_pos + Vec3{ 0, 0.05f, 0.5f });
		game::DrawLine(g_pos + Vec3{ 0, 1.0f, 0 }, g_pos + Vec3{ std::sin(g_heading), 1.0f, std::cos(g_heading) });
	}

	bool  HasEstimate() { return g_estimate; }
	Vec3  Position() { return g_pos; }
	float GroundOffset() { return g_groundOffset; }
	Vec3  DrawShift() { return g_drawShift; }
	float DrawGroundOffset() { return g_drawGroundOffset; }
	Vec3  DrawGroundShift() { return g_drawGroundShift; }
	Vec3  WaitShift() { return g_pinShift; }

	void OnWorldShift(const Vec3& a_delta)
	{
		g_prevPe = g_prevPe + a_delta;
		g_lastMapped = g_lastMapped + a_delta;
		g_placeGuard.Shift(a_delta);
		g_pin.Shift(a_delta);
	}

	void OnOrder(std::uint32_t a_command, std::uint32_t a_buddyType)
	{
		if (a_buddyType != kDog || !Cfg().dog.enabled || !Cfg().dog.waitPin) {
			return;
		}
		if (a_command == 9) {
			if (!g_haveMapped) {
				Log::Info("buddy: D-Dog waits -- not placed yet: no Elden Ring spot to keep");
				return;
			}
			++g_pinWaits;
			g_pin.Begin(g_lastMapped);
			Log::Info("buddy: D-Dog waits at ({:.2f}, {:.2f}) -- kept on that Elden Ring spot (what Snake's slip at Elden Ring's walls explains of its drift is "
			          "taken back; {:.2f} m still easing)",
				g_lastMapped.x, g_lastMapped.z, LengthXZ(g_pin.Shift()));
		} else if ((a_command == 8 || a_command == 16) && g_pin.Waiting()) {
			g_pin.End();
			++g_pinEnds;
			Log::Info("buddy: D-Dog called back (order {}) -- its Wait correction {:.2f} m eases out", a_command, LengthXZ(g_pin.Shift()));
		}
	}

	void OnDogAttack(std::uint32_t a_phase)
	{
		// The dog runs from its Wait to an attack order's target (kDogOrdered: MGSV took the order): the wait is over, the correction
		// goes with the dog's own run (WaitPin) -- it was kept until the dog had moved 1.5 m off the slip's line, and the bite was drawn
		// off its target (review 0.8.0). A refused order leaves the dog waiting in MGSV, and the pin with it.
		if (a_phase != proto::kDogOrdered || !Cfg().dog.enabled || !Cfg().dog.waitPin || !g_pin.Waiting()) {
			return;
		}
		g_pin.End();
		++g_pinEnds;
		Log::Info("buddy: D-Dog runs from its Wait to an attack order's target -- its Wait correction {:.2f} m goes with its run", LengthXZ(g_pin.Shift()));
	}

	std::string Describe()
	{
		const auto& g = Host::Current().guest;
		std::string s = std::format("buddy {} ({}) flags {:#x} object {:#x} ammo {}, Snake {}{}", TypeName(g_type == 0xFFFF ? 0 : g_type), g_type, g_flags,
			g.buddyObjectId, g.buddyAmmo, Puppet::Riding() ? "RIDING" : "on foot", g_absent.load() ? ", NONE OUT (draws left out)" : "");
		if (g_estimate) {
			s += std::format(", placed from {} ({} draws) at ({:.2f}, {:.2f}, {:.2f}) heading {:.2f}, ground offset {:.2f}, drawn shifted {:.2f} m", g_source,
				g_draws, g_pos.x, g_pos.y, g_pos.z, g_heading, g_groundOffset, LengthXZ(g_drawShift));
		} else {
			s += ", not placed (no kBuddyPosValid pose and no kOwnerBuddy draws)";
		}
		s += std::format("; no ground under it: {} moved toward Snake, {} on Snake's level, {} left floating; the drawn body's ground measured apart {} frames",
			g_floorShifts, g_floorSnake, g_floorNone, g_drawGroundApart);
		const auto a = [](buddyguard::Act x) { return g_placeActs[static_cast<std::size_t>(x)]; };
		s += std::format("\nD-Dog's place guard ([Dog] step_guard {}): {} snaps spread, {} jumps held ({} came back, {} taken), largest step raw {:.2f} m, placed {:.2f} m; "
		                 "Wait pin ([Dog] wait_pin {}): {}, {} waits, {} ends, {} resets, now {:.2f} m (largest {:.2f} m; Snake's slip since the Wait {:.2f} m)",
			Cfg().dog.stepGuard ? "on" : "off", a(buddyguard::Act::kSnap), a(buddyguard::Act::kHold), a(buddyguard::Act::kReturned),
			a(buddyguard::Act::kTakenLate) + a(buddyguard::Act::kTakenOut) + a(buddyguard::Act::kEase), g_placeRawMax, g_placeOutMax, Cfg().dog.waitPin ? "on" : "off",
			g_pin.Waiting() ? "WAITING" : "not waiting", g_pinWaits, g_pinEnds, g_pinResets, LengthXZ(g_pinShift), g_pinMax, LengthXZ(g_pin.Slip()));
		return s;
	}
}
