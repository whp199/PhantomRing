#pragma once
// A buddy's own location object (the Fox location class Snake's and every realized character's belongs to: vtable 0x1424C8EF0),
// found and moved the way D-Horse's (Horse.cpp, docs/GUEST.md 8.1a) and D-Walker's (Walker.cpp, 18.3) are: a background scan of
// MGSV's writable memory lists every object of the class, the one whose feet (+0x10) stand on the buddy's reference point (its skin
// root, or GetPosition) is the candidate, confirmed after HorseLogic's Tracker agrees 10 times over 0.5 s, and its setter (vf 0x68,
// Hold's PlaceLocation) moves it. Used by D-Dog (Dog.cpp); Horse.cpp and Walker.cpp keep their own copies of the same steps.
// Game thread, except the scan (its own thread).

#include "HorseLogic.h"
#include "Logic.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace phantom::guest::buddyloc
{
	using logic::Vec3;

	// The location class's own sites (constructor vtables, setter slot, setter): false = never move anything with it.
	bool SitesOk(const char* a_who);

	class Finder
	{
	public:
		explicit Finder(const char* a_who) : m_who(a_who) {}

		// Game thread, each frame. a_ref = the buddy's reference point (nullopt: unknown -- the verdict holds, no scan); a_allowed =
		// scanning and tracking are wanted at all (false forgets the object).
		void Update(const std::optional<Vec3>& a_ref, std::uint64_t a_nowMs, bool a_allowed);
		// The confirmed object (0: none).
		[[nodiscard]] std::uintptr_t Confirmed() const { return m_tracker.Confirmed() ? m_tracker.Loc() : 0; }
		[[nodiscard]] std::uintptr_t Candidate() const { return m_tracker.Loc(); }
		// +0x10 of a location object (nullopt: not one any more).
		static std::optional<Vec3> Feet(std::uintptr_t a_loc);
		static std::optional<std::array<float, 4>> Rotation(std::uintptr_t a_loc);
		// Moves the confirmed object so its feet go from a_feetNow to a_to (the class's lift above the feet kept from +0x20), facing
		// a_rot (nullopt: its own +0x60). Read back (+0x20, 5 cm). Tells the tracker the skin will jump.
		bool Place(const Vec3& a_feetNow, const Vec3& a_to, const std::optional<std::array<float, 4>>& a_rot);
		// The skin will jump with a placement of ours (the tracker's next disagreements don't count).
		void NoteShift() { m_tracker.NoteShift(); }
		void Forget(const char* a_why);
		void RequestScan() { m_scanAsked.store(true); }
		[[nodiscard]] std::uint32_t Scans() const { return m_scans; }
		[[nodiscard]] int           Agreed() const { return m_tracker.Agreed(); }

	private:
		void Scan(const char* a_why, std::uint64_t a_nowMs);
		void Pick(const Vec3& a_ref, std::uint64_t a_nowMs);

		const char*                 m_who;
		horse::plan::Tracker        m_tracker;
		std::mutex                  m_scanLock;
		std::vector<std::uintptr_t> m_scanned;
		std::atomic<bool>           m_scanning{ false };
		std::atomic<bool>           m_scanReady{ false };
		std::atomic<bool>           m_scanAsked{ false };
		std::uint64_t               m_scanAt{ 0 };
		std::uint32_t               m_scans{ 0 };
		bool                        m_lastScanFound{ true };
	};
}
