#pragma once

// Keyboard and mouse play Snake (roadmap 2.14 tier 1; [Keyboard] in PhantomRingHost.ini, off by default; UNVERIFIED live).
//
// While Snake plays, the keys and the mouse become the pad MGSV gets (er/KbmMap.h has the mapping and the mouse curve):
// the bridge's XInputGetState detour (er/InputBridge.cpp) hands it on exactly as a real pad's, so Start/View, the chord,
// the prompt button and MGSV's menu owning the pad all work as on a controller.
//  - Where the keys come from: GetAsyncKeyState once a frame, only while Elden Ring's window is in front (as MenuWatch).
//  - Where the mouse comes from: Elden Ring's own DirectInput mouse (it reads keyboard and mouse through DirectInput8 and
//    has no raw input: docs/research/binary-scout.md). IDirectInputDevice8::GetDeviceState / GetDeviceData are hooked in
//    dinput8.dll (a system DLL, like xinput1_4: Arxan never checks it; the fromsoftware-rs InputBlocker's approach), so the
//    deltas are the device's own counts, never the cursor's position. We register no raw input of our own: under Wine,
//    dinput's mouse may itself sit on raw input, and a second registration in the process would take it over.
//  - What Elden Ring gets while Snake plays: none of the keyboard but [Keyboard] er_keys (Esc = its menu) and none of the
//    mouse, so the hidden Tarnished never rolls on Space or swings on a click; its menus, Tarnished mode and everything
//    else (no Snake) are left alone. In Tarnished mode only the chord key is kept from it (held = the chord; a tap
//    reaches it at the release: G = its map). An interaction's buttons-only phase: er_keys + er_interaction_keys, no
//    mouse. Keys held across any of these changes never reach it as fresh presses, nor lose their release
//    (er/KbmFilter.h).
//  - A real pad keeps priority: the device MGSV's pad comes from is the last one used, the pad on a tie (KbmMap Arbiter).
//  - No controller at all: Elden Ring then never polls XInput, so the bridge turns on its virtual pad (`input virtualpad`)
//    the first time the keyboard is used ([Keyboard] virtual_pad = auto).

#include "KbmMap.h"

#include <cstdint>
#include <string>

namespace phantom::er::Kbm
{
	// Called by InputBridge::Install (the XInput hook is what makes any of this reach MGSV).
	bool Install();
	[[nodiscard]] bool Installed();

	// FrameBegin, after InputBridge::SetPadMode: samples the keys and the mouse, picks the device, builds the pad.
	void Tick();

	// XInputGetState detour (Elden Ring's input thread), for the published pad, before it is published and masked: the
	// keyboard's pad replaces the real one when the keyboard is the device in use (Tarnished mode: only the chord's Start
	// + View are added). Returns a_result, or ERROR_SUCCESS when it put a pad there.
	unsigned long Apply(unsigned long a_index, void* a_xinputState, unsigned long a_result);

	// Elden Ring's window gets this key now (MenuWatch counts only what reaches it).
	[[nodiscard]] bool KeyReachesEr(int a_vk);
	// [Input] pad_glyph_device, or the keyboard's (1) while the keyboard plays Snake ([Keyboard] keyboard_glyphs).
	[[nodiscard]] int GlyphDevice(int a_padDevice);
	[[nodiscard]] bool KeyboardInUse();

	// Console `input kbm [on|off|sens <v>|accel <v>|antidz <v>|smooth <v>]`.
	std::string Command(const std::string& a_sub, const std::string& a_value);
	std::string Describe();
}
