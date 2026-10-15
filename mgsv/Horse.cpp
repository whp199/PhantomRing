// D-Horse treadmill: the game glue (Horse.h; the pure half and its reasons in HorseLogic.h).
#include "Horse.h"

#include "ActorProxy.h"
#include "Addresses.h"
#include "Config.h"
#include "Hold.h"
#include "Hook.h"
#include "Log.h"
#include "Skeleton.h"
#include "Verify.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace phantom::guest::horse
{
	namespace
	{
		using logic::Vec3;

		int  g_sites{ -1 };  // -1 not checked, 0 differ, 1 ok
		bool g_holdTried{ false };

		// The scan (background thread): every location object in MGSV's writable memory. ActorProxy keeps its own list for its
		// soldiers; this one is only read when the horse's object is unknown, so the two never share state.
		std::mutex                  g_scanLock;
		std::vector<std::uintptr_t> g_scanned;
		std::atomic<bool>           g_scanning{ false };
		std::atomic<bool>           g_scanReady{ false };
		std::atomic<bool>           g_scanAsked{ false };
		std::uint64_t               g_scanAt{ 0 };
		std::uint32_t               g_scans{ 0 };
		bool                        g_lastScanFound{ true };

		// Game thread only.
		plan::Tracker g_tracker;
		std::uint16_t g_horseId{ 0xFFFF };  // the horse the tracker is about (a new one starts over)
		bool          g_unmovable{ false };
		int           g_failures{ 0 };
		std::uint64_t g_shifts{ 0 }, g_followed{ 0 }, g_failed{ 0 };
		std::uint64_t g_shiftAtMs{ 0 };      // the last placement, while its skin check is open
		std::string   g_last{ "none" };
		// Ridden jumps: where the last placement took the object from (put back if Snake isn't carried), the window for his
		// landing, and whether a ridden jump has already left him behind (this horse, this session).
		struct Before
		{
			std::uintptr_t       loc{ 0 };
			Vec3                 at;
			std::array<float, 4> rot{};
			Vec3                 root;  // Snake's, at a ridden jump
		};
		Before            g_before;
		plan::RiddenWatch g_watch;
		bool              g_riderLeft{ false };
		// `!horse shift` is refused from the console's thread without a confirmed object: a copy of the tracker's state.
		std::atomic<bool> g_consoleConfirmed{ false };
		// `!horse` runs on the console's thread: it reads this copy, refreshed by the game thread.
		std::mutex    g_describeLock;
		std::string   g_describe{ "horse treadmill: no frame yet" };
		std::uint32_t g_describeTick{ 0 };

		std::mutex          g_reqLock;
		std::optional<Vec3> g_testShift;

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

		// +0x10 the feet, +0x20 the setter's point (feet + the class's lift), +0x60 the rotation (ActorProxy v1, live).
		std::optional<Vec3> Feet(std::uintptr_t a_loc) { return IsLocation(a_loc) ? ReadVec(a_loc + 0x10) : std::nullopt; }

		std::optional<Vec3> Skin()
		{
			Vec3  p;
			float yaw = 0;
			return skeleton::BuddyPose(p, yaw, 500) ? std::optional<Vec3>(p) : std::nullopt;
		}

		bool SitesOk()
		{
			if (g_sites < 0) {
				g_sites = Matches(addr::kLocationCtorVtables, "location class constructor") && Matches(addr::kLocationSetSlot, "location setter slot") &&
				                  Matches(addr::kLocationSet, "location setter")
				              ? 1
				              : 0;
				if (!g_sites) {
					Log::Warn("horse: the location class differs in this exe -- [Buddy] HorseTreadmill stays off");
				}
			}
			return g_sites == 1;
		}

		// As ActorProxy's scan (ActorProxyGame.cpp): every 8-byte aligned qword equal to the location vtable in MGSV's committed
		// private writable memory, read through ReadProcessMemory (a region can go away under the scan). ~2.3 GB in ~220 ms (live).
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
				Log::Debug("horse: scan ({}): {} location objects in {} ms", why, locs.size(), ::GetTickCount64() - t0);
				{
					std::scoped_lock lock(g_scanLock);
					g_scanned = std::move(locs);
				}
				g_scanReady.store(true);
				g_scanning.store(false);
			}).detach();
		}

		// The scan's objects standing where the horse's skin is, minus Snake's own and ActorProxy's soldiers'.
		void Pick(const Vec3& a_skin, std::uint64_t a_nowMs)
		{
			std::vector<std::uintptr_t> locs;
			{
				std::scoped_lock lock(g_scanLock);
				locs = g_scanned;
			}
			std::vector<plan::LocSample> samples;
			std::vector<std::uintptr_t>  exclude;
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
			const auto pick = plan::PickLocation(samples, a_skin, exclude);
			g_lastScanFound = pick != 0;
			if (!pick) {
				Log::Info("horse: none of {} location objects stands on D-Horse's skin at {:.2f},{:.2f},{:.2f} -- looked for again in 30 s", samples.size(),
					a_skin.x, a_skin.y, a_skin.z);
				return;
			}
			g_tracker.Offer(pick, a_nowMs);
			Log::Info("horse: location object {:#x} stands on D-Horse's skin ({} scanned) -- confirming", pick, samples.size());
		}

		void Forget(const char* a_why)
		{
			if (g_tracker.Loc()) {
				Log::Info("horse: location object {:#x} forgotten ({})", g_tracker.Loc(), a_why);
			}
			g_tracker.Reset();
			g_shiftAtMs = 0;
			g_watch.Clear();
			g_before = {};
			g_consoleConfirmed.store(false);
		}

		std::optional<Vec3> TakeTestShift()
		{
			std::scoped_lock lock(g_reqLock);
			return std::exchange(g_testShift, std::nullopt);
		}

		void NoteFailure(const std::string& a_why)
		{
			++g_failed;
			g_last = a_why;
			if (++g_failures >= plan::kMaxFailures && !g_unmovable) {
				g_unmovable = true;
				Log::Warn("horse: {} ({} times in a row) -- D-Horse is left to MGSV again this session (whistled after jumps, no treadmill while "
				          "Snake rides it)",
					a_why, g_failures);
			} else {
				Log::Warn("horse: {}", a_why);
			}
		}

		void Snapshot()
		{
			const auto  loc = g_tracker.Loc();
			const auto  feet = Feet(loc);
			std::string text = std::format("horse treadmill {} (sites {}): location object {:#x} {} (feet {}), {} scans, {} moves ({} followed by the skin, "
			                               "{} failed, {} in a row){}; last: {}",
				Cfg().buddyHorseTreadmill ? "on" : "off", g_sites, loc, g_tracker.Confirmed() ? "confirmed" : loc ? "candidate" : "none",
				feet ? std::format("{:.2f},{:.2f},{:.2f}", feet->x, feet->y, feet->z) : std::string("-"), g_scans, g_shifts, g_followed, g_failed, g_failures,
				g_unmovable ? " -- UNMOVABLE this session" : g_riderLeft ? " -- the saddle didn't carry Snake: no ridden jumps this session" : "", g_last);
			std::scoped_lock lock(g_describeLock);
			g_describe = std::move(text);
		}
	}

	bool Init()
	{
		if (!Cfg().buddyHorseTreadmill) {
			return false;
		}
		if (!hold::Available()) {
			hold::Init();  // PlaceLocation lives there (byte checks only)
		}
		const bool ok = SitesOk() && hold::Available();
		Log::Info("horse: [Buddy] HorseTreadmill {}", ok ? "on (D-Horse moved with the treadmill once its location object is found)" : "off (checks failed)");
		return ok;
	}

	void Update(const state::LuaView& a_lua, std::uint64_t a_nowMs)
	{
		if (++g_describeTick % 15 == 0) {
			Snapshot();
		}
		// A `!horse shift` is served by this frame or dropped: never left queued for a later one (the horse elsewhere by then).
		const auto test = TakeTestShift();
		const auto stop = [&](const char* a_why) {
			// Nothing is tracked while off or without the horse: a tracker frozen here would be trusted after `!horse on`, even
			// across a buddy swap (same GameObject id, its old body back in the pool), and the setter would move whoever owns it now.
			if (g_tracker.Loc()) {
				Forget(a_why);
			}
			g_watch.Clear();
			g_consoleConfirmed.store(false);
			if (test) {
				g_last = std::format("console: not moved ({})", a_why);
			}
		};
		if (!Cfg().buddyHorseTreadmill || !SitesOk()) {
			stop("HorseTreadmill off");
			return;
		}
		if (!hold::Available() && !g_holdTried) {
			g_holdTried = true;  // `!horse on` in a session that never needed Hold's checks
			hold::Init();
		}
		if (!hold::Available()) {
			stop("no native placement");
			return;
		}
		const bool horse = a_lua.InGame() && a_lua.buddyActive == state::kBuddyTypeHorse && a_lua.buddyId != 0xFFFF;
		if (!horse) {
			stop("D-Horse is not out");
			return;
		}
		if (a_lua.buddyId != g_horseId) {
			Forget("another horse");
			g_horseId = a_lua.buddyId;
			g_unmovable = false;
			g_riderLeft = false;
			g_failures = 0;
			g_lastScanFound = true;
		}
		const auto skin = Skin();

		if (g_tracker.Loc()) {
			const bool was = g_tracker.Confirmed();
			const auto loc = g_tracker.Loc();
			const auto v = g_tracker.Observe(Feet(loc), skin, a_nowMs);
			if (v == plan::Tracker::Verdict::kConfirmed && !was) {
				Log::Info("horse: location object {:#x} confirmed ({} checks on the skin) -- the treadmill moves D-Horse now", loc, g_tracker.Agreed());
			} else if (v == plan::Tracker::Verdict::kLost) {
				const bool afterShift = g_shiftAtMs && a_nowMs - g_shiftAtMs < 3000;
				Log::Info("horse: location object {:#x} {} its skin", loc, was ? "no longer follows" : "left");
				if (was && afterShift) {
					NoteFailure("its body didn't follow the location object after a jump");
				}
				if (!was) {
					g_lastScanFound = false;  // a wrong pick (a passer-by's object): the next scan in 30 s, not 5
				}
				g_shiftAtMs = 0;
			}
			// The skin caught up with our last placement: the horse really went there (the old skin stood a whole jump away).
			if (g_shiftAtMs && g_tracker.Confirmed()) {
				const auto f = Feet(loc);
				if (skin && f && (*skin - *f).LengthXZ() <= 0.75f) {
					++g_followed;
					g_failures = 0;
					g_shiftAtMs = 0;
				} else if (a_nowMs - g_shiftAtMs > 3000) {
					g_shiftAtMs = 0;  // no skin published meanwhile: nothing to judge by
				}
			}
		} else if (g_scanReady.exchange(false)) {
			if (skin) {
				Pick(*skin, a_nowMs);
			}
		} else if (skin && !g_scanning.load() && !g_unmovable &&
		           (g_scanAsked.exchange(false) || a_nowMs - g_scanAt > (g_lastScanFound ? 5000u : 30000u))) {
			Scan(g_scans ? "again" : "D-Horse out", a_nowMs);
		}

		g_consoleConfirmed.store(g_tracker.Confirmed());
		if (test) {
			Shift(*test, a_lua.buddyYawValid ? a_lua.buddyYaw : 0.0f, "console");
			Snapshot();
		}
	}

	plan::Action Decide(const state::LuaView& a_lua, bool a_ridden)
	{
		plan::ActionInput in;
		in.enabled = Cfg().buddyHorseTreadmill && g_sites == 1;
		in.horse = a_lua.buddyActive == state::kBuddyTypeHorse && a_lua.buddyId != 0xFFFF;
		in.posValid = a_lua.buddyPosValid;
		in.ridden = a_ridden;
		in.located = g_tracker.Confirmed() && a_lua.buddyId == g_horseId;
		in.unmovable = g_unmovable;
		in.riderLeft = g_riderLeft;
		return plan::Decide(in);
	}

	std::optional<Vec3> Shift(const Vec3& a_shift, float a_fallbackYaw, const char* a_why, std::optional<float> a_groundY)
	{
		const auto loc = g_tracker.Loc();
		if (!g_tracker.Confirmed() || !IsLocation(loc) || g_unmovable) {
			g_last = std::format("{}: not moved (no confirmed location object)", a_why);
			return std::nullopt;
		}
		const auto at = ReadVec(loc + 0x20);
		const auto feet = Feet(loc);
		const auto rot = SafeRead<std::array<float, 4>>(reinterpret_cast<const void*>(loc + 0x60));
		const Vec3 shift = a_groundY && feet ? plan::GroundedShift(a_shift, *feet, *a_groundY) : a_shift;
		const auto p = at && feet && rot ? plan::Shifted(*at, *rot, shift, a_fallbackYaw) : std::nullopt;
		if (!p || !hold::PlaceLocation(loc, p->pos, p->rot)) {
			NoteFailure(std::format("{}: its location object couldn't be read or set", a_why));
			return std::nullopt;
		}
		g_before = { loc, *at, *rot };
		// The setter's own point read back (+0x20: the copy that persisted for soldiers, GUEST.md 20.5).
		const auto after = IsLocation(loc) ? ReadVec(loc + 0x20) : std::nullopt;
		if (!after || !plan::Followed(*at, *after, shift, 0.3f)) {
			NoteFailure(std::format("{}: the setter didn't move the location object ({:.2f},{:.2f} asked)", a_why, shift.x, shift.z));
			return std::nullopt;
		}
		g_tracker.NoteShift();
		g_shiftAtMs = ::GetTickCount64();
		++g_shifts;
		g_last = std::format("{}: feet {:.2f},{:.2f},{:.2f} moved by {:.2f},{:.2f},{:.2f}", a_why, feet->x, feet->y, feet->z, shift.x, shift.y, shift.z);
		Log::Debug("horse: moved by {:.2f},{:.2f},{:.2f} ({})", shift.x, shift.y, shift.z, g_last);
		return p->pos - *at;
	}

	void NoteRiddenJump(std::uint64_t a_frame, std::uint64_t a_nowMs, const Vec3& a_root)
	{
		g_before.root = a_root;
		g_watch.Start(a_frame, a_nowMs);
	}

	void RiddenLanded() { g_watch.Clear(); }

	bool RiddenMissed(std::uint64_t a_frame, std::uint64_t a_nowMs, const Vec3& a_root)
	{
		if (!g_watch.Missed(a_frame, a_nowMs)) {
			return false;
		}
		g_watch.Clear();
		g_riderLeft = true;
		// Back under him (he is still in the saddle of a horse that isn't there), with the heading it had.
		const auto loc = g_before.loc;
		const Vec3 to = plan::BackUnder(g_before.at, g_before.root, a_root);
		const bool back = loc && loc == g_tracker.Loc() && IsLocation(loc) && to.Finite() && hold::PlaceLocation(loc, to, g_before.rot);
		if (back) {
			g_tracker.NoteShift();
			g_shiftAtMs = a_nowMs;
		}
		g_last = std::format("treadmill (ridden): Snake didn't come along -- D-Horse {}", back ? "put back under him" : "NOT put back (object gone)");
		Log::Warn("horse: Snake's root didn't follow the ridden jump within {} frames / {} ms -- the saddle doesn't carry him; D-Horse {}, no "
		          "treadmill while he rides it this session (try [Buddy] HorseMoveRider = true)",
			plan::RiddenWatch::kFrames, plan::RiddenWatch::kMs, back ? "put back under him" : "could NOT be put back (its object is gone)");
		return true;
	}

	std::string Describe()
	{
		std::scoped_lock lock(g_describeLock);
		return g_describe;
	}

	void RequestScan() { g_scanAsked.store(true); }

	std::string RequestTestShift(const Vec3& a_shift)
	{
		if (!Cfg().buddyHorseTreadmill) {
			return "[Buddy] HorseTreadmill is off (!horse on)";
		}
		if (!g_consoleConfirmed.load()) {
			return "D-Horse's location object isn't confirmed (see !horse)";
		}
		std::scoped_lock lock(g_reqLock);
		g_testShift = a_shift;
		return {};
	}
}
