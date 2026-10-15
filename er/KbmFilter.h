#pragma once

// What Elden Ring's own DirectInput keyboard and mouse may see while the keyboard plays Snake: the pure part of the
// GetDeviceState / GetDeviceData detours in er/Kbm.cpp, so tests/test_host_kbm.inc can drive it. No Windows here.
//
// One rule for both ways Elden Ring may read them: it must never see a press it shouldn't act on, and never miss the
// release of a press it did see.
//  - A snapshot (GetDeviceState: 256 key bytes, a mouse's buttons) is filtered by zeroing. Zero is a release, so hiding
//    a key Elden Ring saw down releases it. A key hidden while held stays hidden until let go, whatever changes under it
//    (the scope, MGSV's menu closing): passed mid-hold it would be a fresh press -- Esc that closed MGSV's menu would open
//    Elden Ring's, E that started an interaction would start a second, G held for the chord would open its map.
//  - Buffered events (GetDeviceData) are filtered by dropping. A press passes only if its key is kept at that moment; a
//    release passes if Elden Ring saw the press (or the key is kept anyway); a key it saw down that is hidden now gets a
//    made-up release. Dropping that release instead would leave the key stuck down in Elden Ring: the Tarnished walking
//    on by himself after Snake takes over, or its next menu scrolling on.

#include "KbmMap.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace phantom::er::Kbm
{
	enum class Scope : std::uint8_t
	{
		kNone,         // Elden Ring's keyboard and mouse as they are (its menus, no Snake, [Keyboard] off)
		kChordOnly,    // Tarnished mode: only the chord key is ours
		kButtonsOnly,  // an Elden Ring interaction before its HUD hides: er_keys + er_interaction_keys, no mouse (the pad's
		               // InputBridge::kInteractionButtons for the keyboard: WASD, Space and a click would move, roll or swing
		               // the hidden Tarnished)
		kFull,         // Snake plays: er_keys only (not even those while MGSV's menu is open: Esc is its Start), no mouse
	};

	struct Mode
	{
		Scope scope{ Scope::kNone };
		bool  guestMenu{ false };  // MGSV's own menu has the pad

		bool operator==(const Mode&) const = default;
	};

	// DIK (DirectInput scan code) sets, from the [Keyboard] bindings.
	struct DikTables
	{
		std::array<std::uint8_t, 256> er{};           // er_keys: kept in kFull
		std::array<std::uint8_t, 256> interaction{};  // er_keys + er_interaction_keys: kept in kButtonsOnly
		std::array<std::uint8_t, 256> chord{};        // the chord key(s): hidden in kChordOnly
		int                           tap{ -1 };      // the first chord key's DIK: the Tarnished-mode tap reaches Elden Ring as it

		// a_dik: a virtual-key code -> its DIK, -1 for none (the glue's MapVirtualKey; mouse buttons have none).
		[[nodiscard]] static DikTables Build(const Params& a_params, int (*a_dik)(int));
	};

	// Does Elden Ring's keyboard keep this DIK in this mode (before the held-key rule)?
	[[nodiscard]] bool ErKeeps(const DikTables& a_tables, int a_dik, Mode a_mode);
	// ...and its mouse (movement, wheel and buttons)?
	[[nodiscard]] constexpr bool ErGetsMouse(Scope a_scope) { return a_scope == Scope::kNone || a_scope == Scope::kChordOnly; }

	// GetDeviceState.
	class SnapshotFilter
	{
	public:
		// a_keys: 256 DIK bytes (non-zero = down), filtered in place. a_tap: the chord key's tap is due (it reads as down).
		void Keyboard(std::uint8_t* a_keys, const DikTables& a_tables, Mode a_mode, bool a_tap);
		// a_buttons: a mouse state's a_count button bytes, filtered in place. false = zero its movement and wheel too.
		bool Mouse(std::uint8_t* a_buttons, std::size_t a_count, Scope a_scope);

	private:
		std::array<bool, 256> keys_{};    // hidden at the last read and still held
		std::array<bool, 8>   buttons_{};
	};

	// GetDeviceData: one per device kind (keyboard: DIKs, mouse: buttons 0..7). Copy it for a DIGDD_PEEK read, which must
	// not change what Elden Ring was told.
	class EventFilter
	{
	public:
		// One key or button event, in order: true = hand it to Elden Ring. a_kept: ErKeeps / ErGetsMouse now.
		bool Pass(std::size_t a_index, bool a_down, bool a_kept);

		// After a read's events: the next key Elden Ring still has down that is hidden now (its made-up release, counted as
		// delivered), or -1. a_kept(index) -> bool. Call it while the buffer has room; the rest go out on the next read.
		template <class Kept>
		int NextRelease(Kept&& a_kept)
		{
			for (std::size_t i = 0; i < seen_.size(); ++i) {
				if (seen_[i] && !a_kept(i)) {
					seen_[i] = false;
					return static_cast<int>(i);
				}
			}
			return -1;
		}

		[[nodiscard]] bool SeesDown(std::size_t a_index) const { return a_index < seen_.size() && seen_[a_index]; }

	private:
		std::array<bool, 256> seen_{};  // Elden Ring was handed this key's press and not its release yet
	};
}
