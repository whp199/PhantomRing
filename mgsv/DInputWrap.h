#pragma once
// MGSV creates DirectInput8 only to enumerate game controllers (research §8.2). Wrapping the
// IDirectInput8W it gets makes that enumeration come back empty, so the only pad MGSV knows is the
// virtual XInput one (Pad.h). Keyboard/mouse enumeration passes through untouched.

#include <windows.h>
#include <unknwn.h>

namespace phantom::guest::dinput
{
	// {BF798031-483A-4DA2-AA99-5D64ED369700}
	inline constexpr GUID kIID_IDirectInput8W{ 0xBF798031, 0x483A, 0x4DA2, { 0xAA, 0x99, 0x5D, 0x64, 0xED, 0x36, 0x97, 0x00 } };

	// Takes ownership of a_real's reference; returns an object to hand to the game instead.
	IUnknown* WrapW(IUnknown* a_real);
	// Number of game-controller devices hidden so far (tests and logs).
	unsigned long HiddenDevices();
}
