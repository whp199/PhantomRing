#pragma once
// MGSV's own menus for Elden Ring's input routing (GuestState kGuestInMenu, docs/GUEST.md 18.6).
//
//  * Pause menu (Back): TppPauseMenu +0x38 (Addresses.h), read natively -- MGSV's Lua can't see it, and it stops the
//    player-camera frames, so the GuestState that says "menu" is republished from the Lua thread while it is open.
//  * iDroid (Start held) and popups: TppUiCommand.IsMbDvcTerminalOpened / IsShowPopup (PhantomRing.Tick's MENU bit).
// The flag is set in the first GuestState after a menu opens and cleared in the first one after it closes.

#include <cstdint>
#include <string>

namespace phantom::guest::menu
{
	// DllMain: byte-check the pause-menu getter (a mismatch leaves only the Lua-visible menus).
	void Install();

	// Any thread: is the pause menu open right now (native read, validated each time)?
	[[nodiscard]] bool PauseOpen();
	// Pause menu, or the iDroid / a popup per the freshest Tick. Also published to state::MenuOpen().
	[[nodiscard]] bool Open();

	// MGSV's Lua thread after every Tick: refresh state::MenuOpen() and count open/close transitions.
	void Poll(std::uint64_t a_nowMs);

	std::string Describe();
}
