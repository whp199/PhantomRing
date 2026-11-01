// D-Walker by its own location object: the game glue (Walker.h; the lead's rules in WalkerLogic.h).
#include "Walker.h"

#include "ActorProxy.h"
#include "Addresses.h"
#include "Config.h"
#include "Follow.h"
#include "Hold.h"
#include "Hook.h"
#include "HorseLogic.h"
#include "Log.h"
#include "Verify.h"
#include "WalkerLogic.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace phantom::guest::walker
{
	namespace
	{
		using logic::Vec3;
		namespace hp = horse::plan;

		int  g_sites{ -1 };  // -1 not checked, 0 differ, 1 ok
		bool g_holdTried{ false };

		// The scan (background thread): every location object in MGSV's writable memory (as Horse.cpp's and ActorProxy's).
		std::mutex                  g_scanLock;
		std::vector<std::uintptr_t> g_scanned;
		std::atomic<bool>           g_scanning{ false };
		std::atomic<bool>           g_scanReady{ false };
		std::atomic<bool>           g_scanAsked{ false };
		std::uint64_t               g_scanAt{ 0 };
		std::uint32_t               g_scans{ 0 };
		bool                        g_lastScanFound{ true };

		// Game thread only.
		hp::Tracker       g_tracker;
		std::uint16_t     g_walkerId{ 0xFFFF };
		bool              g_unmovable{ false };  // the setter didn't take, or the lead lost it, too often this session
		int               g_failures{ 0 };
		std::uint64_t     g_shifts{ 0 }, g_shiftFails{ 0 };
		plan::Lead        g_lead;
		std::uint64_t     g_leads{ 0 }, g_leadFrames{ 0 }, g_leadLost{ 0 }, g_leadStops{ 0 };
		std::uint64_t     g_lostAt[3]{};         // the last three "lost" times: three within 20 s = MGSV fights the lead, off this session
		float             g_maxStep{ 0.0f };     // the largest per-frame move the lead made (m)
		std::string       g_last{ "none" };
		std::optional<Vec3> g_feet;              // this frame's, while confirmed
		// A treadmill / Fulton-anchor shift is applied at the next frame: Snake's own placement shows in his root (and his draws)
		// one frame after the call, the walker's in the same frame -- live, the list in between drew the walker the radius away.
		std::optional<Vec3> g_pendingShift;
		std::string         g_pendingWhy;
		std::uint16_t       g_pendingId{ 0xFFFF };
		std::atomic<bool> g_leading{ false };
		std::atomic<std::uint64_t> g_leadingMs{ 0 };
		bool              g_loggedOff{ false };

		// Console (any thread) -> game thread.
		std::mutex          g_reqLock;
		std::optional<Vec3> g_testShift;
		std::atomic<int>    g_leadSwitch{ -1 };      // -1 none, 0 off, 1 on
		std::atomic<int>    g_treadmillSwitch{ -1 };
		std::mutex          g_describeLock;
		std::string         g_describe{ "walker: no frame yet" };
		std::uint32_t       g_describeTick{ 0 };

		std::optional<Vec3> ReadVec(std::uintptr_t a_at)
		{
			const auto v = SafeRead<std::array<float, 3>>(reinterpret_cast<const void*>(a_at));
			if (!v) {
				return std::nullopt;
			}
			const Vec3 p{ (*v)[0], (*v)[1], (*v)[2] };
			return p.Finite() ? std::optional<Vec3>(p) : std::nullopt;
		}

		bool IsLocation(std::uintptr_t a_loc)
		{
			return a_loc && SafeRead<std::uintptr_t>(reinterpret_cast<const void*>(a_loc)).value_or(0) == addr::kLocationVtable;
		}

		// +0x10 the feet (= GetPosition's, live), +0x20 the setter's point, +0x60 the rotation (xyzw).
		std::optional<Vec3> Feet(std::uintptr_t a_loc) { return IsLocation(a_loc) ? ReadVec(a_loc + 0x10) : std::nullopt; }

		bool SitesOk()
		{
			if (g_sites < 0) {
				g_sites = Matches(addr::kLocationCtorVtables, "location class constructor") && Matches(addr::kLocationSetSlot, "location setter slot") &&
				                  Matches(addr::kLocationSet, "location setter")
				              ? 1
				              : 0;
				if (!g_sites) {
					Log::Warn("walker: the location class differs in this exe -- D-Walker is moved by Lua SetPosition and not led");
				}
			}
			return g_sites == 1;
		}

		void Scan(const char* a_why, std::uint64_t a_nowMs)
		{
			if (g_scanning.exchange(true)) {
				return;
			}
			g_scanAt = a_nowMs;
			++g_scans;
			std::thread([why = std::string(a_why)] {
				const auto                  t0 = ::GetTickCount64();
				std::vector<std::uintptr_t> locs;
				std::vector<std::uint64_t>  buf(1 << 16);
				MEMORY_BASIC_INFORMATION    mbi{};
				std::uintptr_t              at = 0x10000;
				while (at < 0x7FFFFFFF0000ull && ::VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == sizeof(mbi)) {
					const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
					const auto size = static_cast<std::uintptr_t>(mbi.RegionSize);
					if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
						size <= (std::uintptr_t{ 1 } << 31)) {
						for (std::uintptr_t off = 0; off < size; off += buf.size() * 8) {
							const auto n = std::min<std::uintptr_t>(buf.size() * 8, size - off);
							SIZE_T     got = 0;
							if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(base + off), buf.data(), n, &got)) {
								continue;
							}
							for (std::size_t i = 0; i < got / 8; ++i) {
								if (buf[i] == addr::kLocationVtable) {
									locs.push_back(base + off + i * 8);
								}
							}
						}
					}
					at = base + size;
				}
				Log::Debug("walker: scan ({}): {} location objects in {} ms", why, locs.size(), ::GetTickCount64() - t0);
				{
					std::scoped_lock lock(g_scanLock);
					g_scanned = std::move(locs);
				}
				g_scanReady.store(true);
				g_scanning.store(false);
			}).detach();
		}

		void Pick(const Vec3& a_at, std::uint64_t a_nowMs)
		{
			std::vector<std::uintptr_t> locs;
			{
				std::scoped_lock lock(g_scanLock);
				locs = g_scanned;
			}
			std::vector<hp::LocSample>  samples;
			std::vector<std::uintptr_t> exclude;
			if (const auto snake = hold::PlayerLocation()) {
				exclude.push_back(snake);
			}
			for (const auto loc : locs) {
				if (const auto f = Feet(loc)) {
					const float at[3] = { f->x, f->y, f->z };
					if (proxy::NearAnyProxy(at, 1.0f)) {
						exclude.push_back(loc);
					}
					samples.push_back({ loc, *f });
				}
			}
			const auto pick = hp::PickLocation(samples, a_at, exclude);
			g_lastScanFound = pick != 0;
			if (!pick) {
				Log::Info("walker: none of {} location objects stands on D-Walker at {:.2f},{:.2f},{:.2f} -- looked for again in 30 s", samples.size(), a_at.x,
					a_at.y, a_at.z);
				return;
			}
			g_tracker.Offer(pick, a_nowMs);
			Log::Info("walker: location object {:#x} stands on D-Walker ({} scanned) -- confirming", pick, samples.size());
		}

		void Forget(const char* a_why)
		{
			if (g_tracker.Loc()) {
				Log::Info("walker: location object {:#x} forgotten ({})", g_tracker.Loc(), a_why);
			}
			if (g_lead.Active()) {
				g_lead.Stop();
				++g_leadStops;
			}
			g_tracker.Reset();
			g_feet.reset();
		}

		void NoteFailure(const std::string& a_why)
		{
			g_last = a_why;
			if (++g_failures >= hp::kMaxFailures && !g_unmovable) {
				g_unmovable = true;
				g_lead.Stop();
				Log::Warn("walker: {} ({} times) -- D-Walker is left to MGSV again this session (Lua SetPosition on treadmill jumps, not led)", a_why,
					g_failures);
			} else {
				Log::Warn("walker: {}", a_why);
			}
		}

		// Its object to a_feet (horizontally; the class's lift above the feet kept from +0x20) facing a_rot.
		bool Place(std::uintptr_t a_loc, const Vec3& a_feetNow, const Vec3& a_to, const std::optional<std::array<float, 4>>& a_rot)
		{
			const auto at = ReadVec(a_loc + 0x20);
			const auto rot = a_rot ? a_rot : SafeRead<std::array<float, 4>>(reinterpret_cast<const void*>(a_loc + 0x60));
			if (!at || !rot) {
				return false;
			}
			const Vec3 point{ at->x + (a_to.x - a_feetNow.x), at->y + (a_to.y - a_feetNow.y), at->z + (a_to.z - a_feetNow.z) };
			if (!hold::PlaceLocation(a_loc, point, *rot)) {
				return false;
			}
			const auto back = ReadVec(a_loc + 0x20);
			return back && (*back - point).LengthXZ() < 0.05f;
		}

		plan::LeadSettings Settings()
		{
			const auto&        c = Cfg();
			plan::LeadSettings s;
			s.side = c.buddyWalkerLeadSide;
			s.ahead = c.buddyWalkerLeadAhead;
			s.tau = c.buddyWalkerLeadTau;
			s.maxSpeed = c.buddyWalkerLeadMaxSpeed;
			s.acquire = c.buddyWalkerLeadAcquire;
			s.turnRate = c.buddyWalkerLeadTurnRate;
			return s;
		}

		void Snapshot()
		{
			const auto  loc = g_tracker.Loc();
			const auto& c = Cfg();
			std::string text = std::format(
				"walker: object {:#x} {} (feet {}), {} scans; treadmill {} ({} moved, {} failed); lead {} (side {:.1f} ahead {:.1f} tau {:.2f} max {:.1f} m/s "
				"turn {:.1f} acquire {:.1f}): {} now, {} leads, {} frames, {} stopped, {} lost, largest step {:.3f} m{}; last: {}",
				loc, g_tracker.Confirmed() ? "confirmed" : loc ? "candidate" : "none",
				g_feet ? std::format("{:.2f},{:.2f},{:.2f}", g_feet->x, g_feet->y, g_feet->z) : std::string("-"), g_scans, c.buddyWalkerTreadmill ? "on" : "off",
				g_shifts, g_shiftFails, c.buddyWalkerLead ? "on" : "off", c.buddyWalkerLeadSide, c.buddyWalkerLeadAhead, c.buddyWalkerLeadTau,
				c.buddyWalkerLeadMaxSpeed, c.buddyWalkerLeadTurnRate, c.buddyWalkerLeadAcquire, g_lead.Active() ? "leading" : "idle", g_leads, g_leadFrames,
				g_leadStops, g_leadLost, g_maxStep, g_unmovable ? " -- UNMOVABLE this session" : "", g_last);
			std::scoped_lock lock(g_describeLock);
			g_describe = std::move(text);
		}

		void ApplySwitches()
		{
			if (const int s = g_leadSwitch.exchange(-1); s >= 0) {
				MutableCfg().buddyWalkerLead = s == 1;
				if (!s) {
					g_lead.Stop();
				}
				Log::Info("walker: lead {} (console)", s ? "on" : "off");
			}
			if (const int s = g_treadmillSwitch.exchange(-1); s >= 0) {
				MutableCfg().buddyWalkerTreadmill = s == 1;
				Log::Info("walker: treadmill moves {} (console)", s ? "by its object" : "by Lua SetPosition");
			}
		}
	}

	void Update(const state::LuaView& a_lua, const FrameIn& a_in)
	{
		if (++g_describeTick % 15 == 0) {
			Snapshot();
		}
		ApplySwitches();
		std::optional<Vec3> test;
		{
			std::scoped_lock lock(g_reqLock);
			test = std::exchange(g_testShift, std::nullopt);
		}
		g_feet.reset();
		const auto idle = [&](const char* a_why) {
			if (const auto shift = std::exchange(g_pendingShift, std::nullopt); shift && a_lua.buddyPosValid && a_lua.buddyId == g_pendingId) {
				const Vec3 b = a_lua.buddyPos + *shift;  // a jump taken last frame: Lua's SetPosition, as before this feature
				state::QueueCommand(std::format("owarp {} {} {} {} {}", a_lua.buddyId, b.x, b.y, b.z, a_lua.buddyYawValid ? a_lua.buddyYaw : 0.0f));
				follow::OnShift(*shift);
			}
			if (g_tracker.Loc()) {
				Forget(a_why);
			}
			if (g_leading.exchange(false)) {
				++g_leadStops;
			}
			if (test) {
				g_last = std::format("console: not moved ({})", a_why);
			}
		};
		const auto& c = Cfg();
		if (!(c.buddyWalkerLead || c.buddyWalkerTreadmill) || !SitesOk()) {
			if (!g_loggedOff && g_sites == 1) {
				g_loggedOff = true;
				Log::Info("walker: [Buddy] WalkerLead and WalkerTreadmill off -- D-Walker as before");
			}
			idle("off");
			return;
		}
		if (!hold::Available() && !g_holdTried) {
			g_holdTried = true;
			hold::Init();
		}
		const bool walker = a_lua.InGame() && a_lua.buddyActive == state::kBuddyTypeWalker && a_lua.buddyId != 0xFFFF &&
		                    (a_lua.buddyId >> 9) == addr::kWalkerBuddyType && !state::BuddyMissing();
		if (!hold::Available() || !walker) {
			idle(!hold::Available() ? "no native placement" : "D-Walker is not out");
			return;
		}
		if (a_lua.buddyId != g_walkerId) {
			Forget("another walker");
			g_walkerId = a_lua.buddyId;
			g_unmovable = false;
			g_failures = 0;
			g_lastScanFound = true;
		}
		const auto ref = a_lua.buddyPosValid ? std::optional<Vec3>(a_lua.buddyPos) : std::nullopt;

		// Track: while it isn't led, its object must keep standing on GetPosition (30 Hz, a frame or two late: the tracker's
		// 0.75 m window). While led, GetPosition follows our placements (or the lead's own check loses it).
		if (const auto loc = g_tracker.Loc()) {
			const bool was = g_tracker.Confirmed();
			if (g_lead.Active()) {
				g_tracker.NoteShift();
			}
			const auto v = g_tracker.Observe(Feet(loc), ref, a_in.nowMs);
			if (v == hp::Tracker::Verdict::kConfirmed && !was) {
				Log::Info("walker: location object {:#x} confirmed ({} checks on D-Walker) -- moved natively now", loc, g_tracker.Agreed());
			} else if (v == hp::Tracker::Verdict::kLost) {
				Log::Info("walker: location object {:#x} {} D-Walker", loc, was ? "no longer follows" : "left");
				if (!was) {
					g_lastScanFound = false;
				}
				Forget("lost");
			}
		} else if (g_scanReady.exchange(false)) {
			if (ref) {
				Pick(*ref, a_in.nowMs);
			}
		} else if (ref && !g_scanning.load() && !g_unmovable && (g_scanAsked.exchange(false) || a_in.nowMs - g_scanAt > (g_lastScanFound ? 5000u : 30000u))) {
			Scan(g_scans ? "again" : "D-Walker out", a_in.nowMs);
		}

		const auto loc = g_tracker.Confirmed() ? g_tracker.Loc() : 0;
		auto       feet = loc ? Feet(loc) : std::nullopt;
		if (const auto shift = std::exchange(g_pendingShift, std::nullopt)) {
			// Last frame's treadmill jump, now that Snake's root has landed: the walker by the same shift, the lead point with it.
			if (feet && Place(loc, *feet, *feet + *shift, std::nullopt)) {
				g_lead.Shift(*shift);
				g_tracker.NoteShift();
				follow::OnShift(*shift);  // where it walks to, in the same frame (its AI stopped at a destination left behind it)
				++g_shifts;
				feet = *feet + *shift;
				Log::Debug("walker: moved by {:.2f},{:.2f},{:.2f} ({})", shift->x, shift->y, shift->z, g_pendingWhy);
			} else {
				// Its object went (or the setter failed) since the jump was taken: Lua's SetPosition, as before this feature.
				++g_shiftFails;
				g_lead.Stop();
				if (a_lua.buddyPosValid && g_pendingId == a_lua.buddyId) {
					const Vec3 b = a_lua.buddyPos + *shift;
					state::QueueCommand(std::format("owarp {} {} {} {} {}", a_lua.buddyId, b.x, b.y, b.z, a_lua.buddyYawValid ? a_lua.buddyYaw : 0.0f));
				}
				follow::OnShift(*shift);
				NoteFailure(std::format("{}: D-Walker's object couldn't be moved -- Lua SetPosition instead", g_pendingWhy));
			}
		}
		if (feet) {
			g_feet = feet;
		}
		if (test) {
			if (!feet) {
				g_last = "console: not moved (no confirmed object)";
			} else {
				const Vec3 to = *feet + Vec3{ test->x, 0.0f, test->z };
				const bool ok = Place(loc, *feet, to, std::nullopt);
				g_last = std::format("console: {} by {:.2f},{:.2f} (feet {:.2f},{:.2f},{:.2f})", ok ? "moved" : "NOT moved", test->x, test->z, feet->x, feet->y, feet->z);
				Log::Info("walker: {}", g_last);
			}
		}

		// The lead. Allowed only where Follow.cpp's auto-follow acts (Normal Mode or the FollowModes, following on, no player hold,
		// not ridden, nothing busy) and Snake is on foot and free here.
		plan::LeadInput in;
		in.allowed = c.buddyWalkerLead && feet && !g_unmovable && a_in.ready && follow::LeadAllowed() && !test;
		in.dt = a_in.dt;
		in.nowMs = a_in.nowMs;
		in.snake = a_in.root;
		in.velocity = a_in.velocity;
		{
			const float dx = a_in.root.x - a_in.camPos.x, dz = a_in.root.z - a_in.camPos.z;
			in.viewYaw = dx * dx + dz * dz > 0.25f ? std::atan2(dx, dz) : a_in.yaw;
		}
		std::array<float, 4> rotNow{ 0.0f, 0.0f, 0.0f, 1.0f };
		if (feet) {
			in.walker = *feet;
			if (const auto r = SafeRead<std::array<float, 4>>(reinterpret_cast<const void*>(loc + 0x60))) {
				rotNow = *r;
			}
			in.walkerYaw = logic::QuatYaw(rotNow.data());
		}
		const auto out = g_lead.Update(in, Settings());
		if (out.started) {
			++g_leads;
			Log::Debug("walker: lead #{} from {:.1f} m off its spot (Snake {:.1f} m/s)", g_leads, (in.walker - plan::LeadSpot(in, Settings())).LengthXZ(),
				in.velocity.LengthXZ());
		}
		if (out.lost) {
			++g_leadLost;
			std::rotate(std::begin(g_lostAt), std::begin(g_lostAt) + 1, std::end(g_lostAt));
			g_lostAt[2] = a_in.nowMs;
			Log::Info("walker: the lead lost D-Walker (it stood {:.1f} m from where it was put) -- MGSV's again", (in.walker - g_lead.Point()).LengthXZ());
			if (g_lostAt[0] && a_in.nowMs - g_lostAt[0] < 20000) {
				NoteFailure("three leads lost within 20 s: MGSV puts the walker back");
				std::fill(std::begin(g_lostAt), std::end(g_lostAt), 0);
			}
		}
		if (out.stopped && !out.lost) {
			++g_leadStops;
		}
		bool placed = false;
		if (out.place && feet) {
			const float step = (out.to - *feet).LengthXZ();
			const auto  rot = c.buddyWalkerLeadTurnRate > 0.0f ? std::optional<std::array<float, 4>>(logic::YawQuat(out.yaw)) : std::nullopt;
			if (Place(loc, *feet, { out.to.x, feet->y, out.to.z }, rot)) {
				placed = true;
				++g_leadFrames;
				g_maxStep = std::max(g_maxStep, step);
				g_feet = Vec3{ out.to.x, feet->y, out.to.z };
			} else {
				g_lead.Stop();
				++g_leadStops;
				NoteFailure("the setter didn't take a lead placement");
			}
		}
		g_leading.store(placed || g_lead.Active());
		if (placed) {
			g_leadingMs.store(a_in.nowMs);
		}
	}

	bool Shift(const Vec3& a_shift, const char* a_why)
	{
		const auto loc = g_tracker.Confirmed() ? g_tracker.Loc() : 0;
		if (!Cfg().buddyWalkerTreadmill || !IsLocation(loc) || g_unmovable || !a_shift.Finite()) {
			return false;
		}
		// Applied at the next frame (Update), when Snake's own placement has landed in his root.
		g_pendingShift = g_pendingShift ? *g_pendingShift + a_shift : a_shift;
		g_pendingWhy = a_why;
		g_pendingId = g_walkerId;
		return true;
	}

	std::optional<Vec3> Position() { return g_feet; }

	bool Leading() { return g_leading.load() || ::GetTickCount64() - g_leadingMs.load() < 300; }

	std::string Command(const std::string& a_args)
	{
		if (a_args == "scan") {
			g_scanAsked.store(true);
			return "walker: scan asked";
		}
		if (a_args == "lead on" || a_args == "lead off") {
			g_leadSwitch.store(a_args == "lead on" ? 1 : 0);
			return "walker: " + a_args;
		}
		if (a_args == "treadmill on" || a_args == "treadmill off") {
			g_treadmillSwitch.store(a_args == "treadmill on" ? 1 : 0);
			return "walker: " + a_args;
		}
		if (a_args.starts_with("lead ")) {
			float side = 0, ahead = 0, tau = 0, maxSpeed = 0, turn = -1, acquire = -1;
			const int n = std::sscanf(a_args.c_str() + 5, "%f %f %f %f %f %f", &side, &ahead, &tau, &maxSpeed, &turn, &acquire);
			if (n < 4) {
				return "usage: !walker lead <side> <ahead> <tau s> <max m/s> [turn rad/s] [acquire m]";
			}
			auto& m = MutableCfg();
			m.buddyWalkerLeadSide = std::clamp(side, -10.0f, 10.0f);
			m.buddyWalkerLeadAhead = std::clamp(ahead, -10.0f, 10.0f);
			m.buddyWalkerLeadTau = std::clamp(tau, 0.05f, 5.0f);
			m.buddyWalkerLeadMaxSpeed = std::clamp(maxSpeed, 1.0f, 25.0f);
			if (n >= 5) {
				m.buddyWalkerLeadTurnRate = std::clamp(turn, 0.0f, 30.0f);
			}
			if (n >= 6) {
				m.buddyWalkerLeadAcquire = std::clamp(acquire, 1.0f, 30.0f);
			}
			Log::Info("walker: lead side {:.1f} ahead {:.1f} tau {:.2f} max {:.1f} turn {:.1f} acquire {:.1f} (console)", m.buddyWalkerLeadSide,
				m.buddyWalkerLeadAhead, m.buddyWalkerLeadTau, m.buddyWalkerLeadMaxSpeed, m.buddyWalkerLeadTurnRate, m.buddyWalkerLeadAcquire);
			return "walker: lead settings set";
		}
		if (a_args.starts_with("shift ")) {
			float dx = 0, dz = 0;
			if (std::sscanf(a_args.c_str() + 6, "%f %f", &dx, &dz) != 2) {
				return "usage: !walker shift <dx> <dz>";
			}
			std::scoped_lock lock(g_reqLock);
			g_testShift = Vec3{ dx, 0.0f, dz };
			return "walker: shift queued for the next frame";
		}
		return Describe();
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_describeLock);
		return g_describe;
	}
}
