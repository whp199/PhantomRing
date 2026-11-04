// A buddy's location object: the finder and the setter (BuddyLocation.h).
#include "BuddyLocation.h"

#include "ActorProxy.h"
#include "Addresses.h"
#include "Hold.h"
#include "Hook.h"
#include "Log.h"
#include "Verify.h"

#include <windows.h>

#include <algorithm>
#include <format>
#include <thread>

namespace phantom::guest::buddyloc
{
	namespace
	{
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
	}

	bool SitesOk(const char* a_who)
	{
		static int sites = -1;  // -1 not checked, 0 differ, 1 ok (the same three sites for every buddy)
		if (sites < 0) {
			sites = Matches(addr::kLocationCtorVtables, "location class constructor") && Matches(addr::kLocationSetSlot, "location setter slot") &&
			                Matches(addr::kLocationSet, "location setter")
			            ? 1
			            : 0;
			if (!sites) {
				Log::Warn("{}: the location class differs in this exe -- its location object is never used", a_who);
			}
		}
		return sites == 1;
	}

	std::optional<Vec3> Finder::Feet(std::uintptr_t a_loc) { return IsLocation(a_loc) ? ReadVec(a_loc + 0x10) : std::nullopt; }

	std::optional<std::array<float, 4>> Finder::Rotation(std::uintptr_t a_loc)
	{
		return IsLocation(a_loc) ? SafeRead<std::array<float, 4>>(reinterpret_cast<const void*>(a_loc + 0x60)) : std::nullopt;
	}

	void Finder::Scan(const char* a_why, std::uint64_t a_nowMs)
	{
		if (m_scanning.exchange(true)) {
			return;
		}
		m_scanAt = a_nowMs;
		++m_scans;
		// The finder is a static object of its owner (lives for the whole process): the thread may use it.
		std::thread([this, why = std::string(a_why)] {
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
			Log::Debug("{}: scan ({}): {} location objects in {} ms", m_who, why, locs.size(), ::GetTickCount64() - t0);
			{
				std::scoped_lock lock(m_scanLock);
				m_scanned = std::move(locs);
			}
			m_scanReady.store(true);
			m_scanning.store(false);
		}).detach();
	}

	void Finder::Pick(const Vec3& a_ref, std::uint64_t a_nowMs)
	{
		std::vector<std::uintptr_t> locs;
		{
			std::scoped_lock lock(m_scanLock);
			locs = m_scanned;
		}
		std::vector<horse::plan::LocSample> samples;
		std::vector<std::uintptr_t>         exclude;
		if (const auto snake = hold::PlayerLocation()) {
			exclude.push_back(snake);
		}
		for (const auto loc : locs) {
			if (const auto f = Feet(loc)) {
				const float at[3] = { f->x, f->y, f->z };
				if (proxy::NearAnyProxy(at, 1.0f)) {
					exclude.push_back(loc);  // a proxy soldier's (the dog may stand right at one)
				}
				samples.push_back({ loc, *f });
			}
		}
		const auto pick = horse::plan::PickLocation(samples, a_ref, exclude);
		m_lastScanFound = pick != 0;
		if (!pick) {
			Log::Info("{}: none of {} location objects stands on it at {:.2f},{:.2f},{:.2f} -- looked for again in 30 s", m_who, samples.size(), a_ref.x,
				a_ref.y, a_ref.z);
			return;
		}
		m_tracker.Offer(pick, a_nowMs);
		Log::Info("{}: location object {:#x} stands on it ({} scanned) -- confirming", m_who, pick, samples.size());
	}

	void Finder::Forget(const char* a_why)
	{
		if (m_tracker.Loc()) {
			Log::Info("{}: location object {:#x} forgotten ({})", m_who, m_tracker.Loc(), a_why);
		}
		m_tracker.Reset();
	}

	void Finder::Update(const std::optional<Vec3>& a_ref, std::uint64_t a_nowMs, bool a_allowed)
	{
		if (!a_allowed) {
			if (m_tracker.Loc()) {
				Forget("not wanted now");
			}
			return;
		}
		if (const auto loc = m_tracker.Loc()) {
			const bool was = m_tracker.Confirmed();
			const auto v = m_tracker.Observe(Feet(loc), a_ref, a_nowMs);
			if (v == horse::plan::Tracker::Verdict::kConfirmed && !was) {
				Log::Info("{}: location object {:#x} confirmed ({} checks on it)", m_who, loc, m_tracker.Agreed());
			} else if (v == horse::plan::Tracker::Verdict::kLost) {
				Log::Info("{}: location object {:#x} {} it", m_who, loc, was ? "no longer follows" : "left");
				if (!was) {
					m_lastScanFound = false;  // a passer-by's: the next scan in 30 s
				}
			}
		} else if (m_scanReady.exchange(false)) {
			if (a_ref) {
				Pick(*a_ref, a_nowMs);
			}
		} else if (a_ref && !m_scanning.load() && (m_scanAsked.exchange(false) || a_nowMs - m_scanAt > (m_lastScanFound ? 5000u : 30000u))) {
			Scan(m_scans ? "again" : "out", a_nowMs);
		}
	}

	bool Finder::Place(const Vec3& a_feetNow, const Vec3& a_to, const std::optional<std::array<float, 4>>& a_rot)
	{
		const auto loc = Confirmed();
		if (!loc || !hold::Available()) {
			return false;
		}
		const auto at = ReadVec(loc + 0x20);
		const auto rot = a_rot ? a_rot : Rotation(loc);
		if (!at || !rot) {
			return false;
		}
		const Vec3 point = *at + (a_to - a_feetNow);
		if (!point.Finite() || !hold::PlaceLocation(loc, point, *rot)) {
			return false;
		}
		const auto back = IsLocation(loc) ? ReadVec(loc + 0x20) : std::nullopt;
		if (!back || (*back - point).LengthXZ() >= 0.05f) {
			return false;
		}
		m_tracker.NoteShift();
		return true;
	}
}
