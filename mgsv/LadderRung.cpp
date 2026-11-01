#include "LadderRung.h"

#include "Addresses.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <format>
#include <thread>
#include <vector>

namespace phantom::guest::ladder_rung
{
	namespace
	{
		using logic::Vec3;

		std::atomic<bool>           g_ok{ false };
		std::atomic<bool>           g_searching{ false };
		std::atomic<std::uintptr_t> g_system{ 0 };
		std::atomic<std::uint64_t>  g_searches{ 0 }, g_found{ 0 }, g_shifts{ 0 }, g_shiftRefused{ 0 }, g_lost{ 0 };
		std::atomic<std::uint64_t>  g_lastSearchMs{ 0 };
		std::atomic<std::uint64_t>  g_nextSearchAt{ 0 };  // GetTickCount64: a failed search waits 30 s before the next
		std::atomic<int>            g_candidates{ 0 };

		template <class T>
		std::optional<T> Get(std::uintptr_t a_at)
		{
			return SafeRead<T>(reinterpret_cast<const void*>(a_at));
		}

		std::uintptr_t Impl() { return Get<std::uintptr_t>(addr::kPlayer2System + 8).value_or(0); }

		// Is a_s the player's ladder system? Its four vtables and the player context it shares with Player2SystemImpl.
		bool IsSystem(std::uintptr_t a_s, std::uintptr_t a_context)
		{
			return a_s && Get<std::uintptr_t>(a_s).value_or(0) == addr::kLadderVtable &&
			       Get<std::uintptr_t>(a_s + 0x20).value_or(0) == addr::kLadderVtable2 &&
			       Get<std::uintptr_t>(a_s + 0x38).value_or(0) == addr::kLadderVtable3 &&
			       Get<std::uintptr_t>(a_s + 0x50).value_or(0) == addr::kLadderVtable4 && a_context &&
			       Get<std::uintptr_t>(a_s + addr::kLadderSysContext).value_or(0) == a_context &&
			       Get<std::uintptr_t>(a_s + addr::kLadderSysRecords).value_or(0) != 0;
		}

		void Search()
		{
			const auto t0 = std::chrono::steady_clock::now();
			++g_searches;
			const auto impl = Impl();
			const auto context = impl ? Get<std::uintptr_t>(impl + addr::kImplContext).value_or(0) : 0;
			std::uintptr_t found = 0;
			int            candidates = 0;
			std::uint64_t  bytes = 0;
			if (context) {
				std::vector<std::uint8_t> buf(16u << 20);
				std::uintptr_t            at = 0x10000;
				MEMORY_BASIC_INFORMATION  mbi{};
				while (at < 0x7FFFFFFF0000ull && ::VirtualQuery(reinterpret_cast<void*>(at), &mbi, sizeof(mbi)) == sizeof(mbi)) {
					const auto base = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress);
					const auto next = base + mbi.RegionSize;
					if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
						for (std::uintptr_t off = 0; off < mbi.RegionSize && !found; off += buf.size()) {
							const std::size_t n = std::min<std::size_t>(buf.size(), mbi.RegionSize - off);
							SIZE_T            got = 0;
							if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(base + off), buf.data(), n, &got)) {
								continue;
							}
							bytes += got;
							for (std::size_t i = 0; i + 8 <= got; i += 8) {
								std::uint64_t q;
								std::memcpy(&q, buf.data() + i, 8);
								if (q == addr::kLadderVtable) {
									++candidates;
									if (IsSystem(base + off + i, context)) {
										found = base + off + i;
										break;
									}
								}
							}
						}
					}
					if (found || next <= at) {
						break;
					}
					at = next;
				}
			}
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
			g_lastSearchMs = static_cast<std::uint64_t>(ms);
			g_candidates = candidates;
			if (found) {
				g_system = found;
				++g_found;
				Log::Info("ladder rungs: MGSV's ladder system at {:#x} ({} vtable matches, {:.0f} MB searched in {} ms) -- long ladders without "
				          "climb-overs",
					found, candidates, static_cast<double>(bytes) / 1048576.0, ms);
			} else {
				g_nextSearchAt = ::GetTickCount64() + 30000;
				if (g_searches <= 5) {
					Log::Info("ladder rungs: MGSV's ladder system not found ({}; {} vtable matches, {:.0f} MB in {} ms) -- long ladders loop through "
					          "MGSV's ends; next search in 30 s", context ? "no match with the player's context" : "no Player2System yet", candidates,
						static_cast<double>(bytes) / 1048576.0, ms);
				}
			}
			g_searching = false;
		}

		struct Record
		{
			std::uintptr_t rec{ 0 };
			std::uintptr_t ladder{ 0 };
		};

		std::optional<Record> Resolve(const Vec3& a_foot)
		{
			const auto s = g_system.load(std::memory_order_acquire);
			const auto impl = Impl();
			const auto context = impl ? Get<std::uintptr_t>(impl + addr::kImplContext).value_or(0) : 0;
			if (!s) {
				return std::nullopt;
			}
			if (!IsSystem(s, context)) {
				// A new mission (or MGSV rebuilt its systems): look again.
				g_system = 0;
				++g_lost;
				Log::Info("ladder rungs: the ladder system at {:#x} no longer checks out -- searching again", s);
				Prepare();
				return std::nullopt;
			}
			const auto index = Get<std::uint32_t>(impl + addr::kImplPlayerIndex);
			const auto firstHolder = Get<std::uintptr_t>(s + addr::kLadderSysFirst).value_or(0);
			const auto first = firstHolder ? Get<std::uint32_t>(firstHolder + 0x24) : std::nullopt;
			const auto records = Get<std::uintptr_t>(s + addr::kLadderSysRecords).value_or(0);
			if (!index || !first || !records || *index < *first || *index - *first >= 4) {
				return std::nullopt;
			}
			Record r;
			r.rec = records + std::uintptr_t(*index - *first) * addr::kLadderRecBytes;
			r.ladder = Get<std::uintptr_t>(r.rec + addr::kLadderRecLadder).value_or(0);
			if (!r.ladder) {
				return std::nullopt;
			}
			const auto x = Get<float>(r.ladder + addr::kLadderFoot), y = Get<float>(r.ladder + addr::kLadderFoot + 4),
			           z = Get<float>(r.ladder + addr::kLadderFoot + 8);
			if (!x || !y || !z || (Vec3{ *x, *y, *z } - a_foot).Length() > 1.5f) {
				return std::nullopt;  // another ladder (or a stale record): never touched
			}
			return r;
		}
	}

	bool Init()
	{
		const bool ok = Matches(addr::kLadderRecord, "ladder record") && Matches(addr::kLadderRungAnchor, "ladder rung anchor") &&
		                Matches(addr::kLadderTargetStore, "ladder target store") && Matches(addr::kLadderTopRung, "ladder top rung") &&
		                Matches(addr::kLadderFootRead, "ladder foot read") && Matches(addr::kLadderYawRead, "ladder yaw read") &&
		                Matches(addr::kLadderSystemVtable, "ladder system vtable") && Matches(addr::kLadderSystemVtable2, "ladder system vtable 2") &&
		                Matches(addr::kAmmoComponentLookup, "player index (ladder)") && Matches(addr::kPlayer2SystemAccessor, "Player2System (ladder)");
		g_ok = ok;
		Log::Info("ladder rungs: {}", ok ? "sites verified -- long ladders keep MGSV's climb on its ladder (no climb-overs)"
		                                 : "sites differ -- long ladders loop through MGSV's ends (climb over / step off, mounted again)");
		return ok;
	}

	bool Available() { return g_ok.load(std::memory_order_acquire); }

	void Prepare()
	{
		if (!Available() || g_system.load(std::memory_order_acquire) || ::GetTickCount64() < g_nextSearchAt.load(std::memory_order_acquire) ||
			g_searching.exchange(true)) {
			return;
		}
		std::thread(Search).detach();
	}

	std::optional<Rungs> Read(const Vec3& a_foot)
	{
		if (!Available()) {
			return std::nullopt;
		}
		const auto r = Resolve(a_foot);
		if (!r) {
			return std::nullopt;
		}
		const auto pair = Get<std::uint32_t>(r->rec + addr::kLadderRecRung);
		const auto top = Get<std::uint16_t>(r->ladder + addr::kLadderTop);
		if (!pair || !top) {
			return std::nullopt;
		}
		Rungs out;
		out.rung = static_cast<int>(*pair & 0xFFFF);
		out.target = static_cast<int>(*pair >> 16);
		out.top = *top;
		return out;
	}

	bool Shift(const Vec3& a_foot, const Rungs& a_seen, int a_delta)
	{
		const auto r = Available() ? Resolve(a_foot) : std::nullopt;
		const int  rung = a_seen.rung + a_delta, target = a_seen.target + a_delta;
		if (!r || a_seen.rung < 0 || a_seen.target < 0 || rung < 1 || target < 1 || rung > 0xFFFF || target > 0xFFFF) {
			++g_shiftRefused;
			return false;
		}
		// Both u16 in one aligned store, only if MGSV hasn't moved either since a_seen (its ladder state runs on the game's update).
		auto*         p = reinterpret_cast<std::uint32_t*>(r->rec + addr::kLadderRecRung);
		std::uint32_t expected = static_cast<std::uint32_t>(a_seen.rung) | static_cast<std::uint32_t>(a_seen.target) << 16;
		const auto    desired = static_cast<std::uint32_t>(rung) | static_cast<std::uint32_t>(target) << 16;
		if (!__atomic_compare_exchange_n(p, &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
			++g_shiftRefused;
			return false;
		}
		++g_shifts;
		return true;
	}

	std::string Describe()
	{
		return std::format("ladder rungs: {}, system {:#x} ({} searches, {} found, last {} ms, {} vtable matches, lost {}), {} shifts ({} refused)",
			Available() ? "on" : "off (sites differ)", g_system.load(), g_searches.load(), g_found.load(), g_lastSearchMs.load(), g_candidates.load(),
			g_lost.load(), g_shifts.load(), g_shiftRefused.load());
	}
}
