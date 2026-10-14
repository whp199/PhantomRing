#include "Buddy.h"

#include "Config.h"
#include "Game.h"
#include "Host.h"
#include "Hud.h"
#include "Link.h"
#include "Log.h"
#include "Puppet.h"

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
		// He walks on MGSV's flat sandbox: snap his feet to Elden Ring's ground under him.
		if (game::CastRay(er + Vec3{ 0, 2.5f, 0 }, Vec3{ 0, -8.0f, 0 }, hit, a_f.player.chr, Cfg().rayFilter)) {
			g_groundOffset = hit.y - er.y;
			er.y = hit.y;
		} else {
			g_groundOffset = 0;
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

	std::string Describe()
	{
		const auto& g = Host::Current().guest;
		std::string s = std::format("buddy {} ({}) flags {:#x} object {:#x} ammo {}, Snake {}{}", TypeName(g_type == 0xFFFF ? 0 : g_type), g_type, g_flags,
			g.buddyObjectId, g.buddyAmmo, Puppet::Riding() ? "RIDING" : "on foot", g_absent.load() ? ", NONE OUT (draws left out)" : "");
		if (g_estimate) {
			s += std::format(", placed from {} ({} draws) at ({:.2f}, {:.2f}, {:.2f}) heading {:.2f}, ground offset {:.2f}", g_source, g_draws, g_pos.x,
				g_pos.y, g_pos.z, g_heading, g_groundOffset);
		} else {
			s += ", not placed (no kBuddyPosValid pose and no kOwnerBuddy draws)";
		}
		return s;
	}
}
