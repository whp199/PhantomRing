#pragma once
// C4's detonator (docs/GUEST.md 18.8a, [Combat] C4Detonator): the player sets off Snake's placed C4 with MGSV's own button
// -- ACTION (Y by default) with C4 in hand -- and its blast goes to Elden Ring where the host's copy lies.
//
// MGSV's chain (static RE, Addresses.h "C4's detonator"): the press sets a request bit, the player's action state calls
// PlayerDetonate, which asks the Placed System to set off the oldest C4 of the kind in hand (DetonateOwn -> SetOff). In the
// sandbox no C4 ever went off that way (GUEST.md 18.8), so the guest watches every step: MGSV's SetOff of Snake's C4 marks it
// as the player's (Placed.cpp); PlayerDetonate refusing a request (its status bits are logged) makes the guest set the oldest
// off at once; a tap MGSV never answers within [Combat] C4GraceMs is only logged, or set off by the guest with [Combat]
// C4Backup (off: MGSV's Y is also Fulton, supply boxes and D-Walker). One press, one C4, as in MGSV ([Combat] C4DetonateAll:
// every one).

#include <cstdint>
#include <string>

namespace phantom::guest::c4
{
	// Placed::Init, after the SetOff detour went in: the PlayerDetonate detour (byte-checked; refused = no immediate answer to a
	// refused press, the grace-time fallback still works).
	void Init();

	// Game thread, every player-camera frame (Frame.cpp): a_buttons = PlayerVars.scannedButtonsDirect, a_inHand =
	// vars.currentInventorySlot, a_support = the selected support item's equip, a_free = on foot, no menu.
	void Update(bool a_ready, std::uint32_t a_buttons, std::uint32_t a_inHand, std::uint16_t a_support, bool a_free);

	// Placed.cpp's SetOff detour (its lock released): MGSV set off one of Snake's C4 that reached Elden Ring -- never an old
	// save's (a_detonator: from DetonateOwn, i.e. its own detonator; a_equip: that C4's grade, for [Combat] C4DetonateAll).
	void NoteMgsvSetOff(bool a_detonator, std::uint16_t a_equip);

	std::string Describe();
}
