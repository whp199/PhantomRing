#pragma once
// MGSV's own HUD as a transparent layer for Elden Ring (DESIGN §9, SkyCraft's GUI layer): aim reticle,
// weapon/ammo box, notifications, prompts, marking, iDroid -- whatever MGSV's UI system draws.
//
// How (docs/GUEST.md §16):
//  - MGSV draws its UI from native UI render jobs: the job executor 0x1402E7770 (MGS5VR ui_renderer.cpp,
//    MIT) runs each UI job on a worker thread, and every D3D11 draw that thread issues while inside it
//    is a UI draw. A thread-local depth counter around the executor classifies draws exactly, without
//    guessing from shaders or render targets.
//  - Each UI draw is duplicated, on the context that records it, into a render target of our own (same
//    size and format as the game's UI target, cleared to transparent black every frame) with a blend
//    state derived from the game's: colour blends as MGSV blends it, alpha accumulates coverage
//    ("over": ONE, INV_SRC_ALPHA). The result is MGSV's HUD with premultiplied alpha, exactly as MGSV
//    composites it over its own scene, minus the scene.
//  - At Present the target is copied to a staging texture and cleared (immediate context, ours there),
//    and a copy from an earlier Present is mapped without waiting and written into the HUD triple buffer
//    (protocol HudCtl / HudSlotHdr / kOffHudSlots): RGBA8 rows, premultiplied (flags bit 0), and
//    sRGB-encoded when MGSV's target is (flags bit 1).
// Failures (bytes differ, a D3D call fails) turn the HUD layer off and leave the game untouched.

#include "D3D11Hooks.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace phantom::guest::capture::hud
{
	// HudSlotHdr::flags bits this guest writes (protocol: bit 0 = premultiplied alpha).
	inline constexpr std::uint32_t kHudPremultiplied = 1u << 0;
	inline constexpr std::uint32_t kHudSrgb = 1u << 1;  // colour bytes are sRGB-encoded (sample as *_SRGB)

	// DllMain: byte-check and detour the UI job executor (created disabled; Hooks::EnableAll).
	void InstallEarly();

	// True while this thread runs one of MGSV's UI jobs (any depth).
	[[nodiscard]] bool InUiJob();

	// Recording thread (D3D11Hooks draw callback): a draw MGSV records. Duplicates UI draws.
	void OnDraw(d3d::ContextState& a_state, const d3d::DrawArgs& a_args);
	// Presenting thread, before the game's Present: read back, publish, clear for the next frame.
	void OnPresent(ID3D11Device* a_device, ID3D11DeviceContext* a_immediate);

	// "!capture hud [on|off|every N|probe]" (Capture.cpp forwards).
	std::string Command(std::string_view a_args);
	std::string Describe();

	// One line describing a UI draw for the capture probe (render target, blend, shaders).
	std::string ProbeLine(const d3d::ContextState& a_state, const d3d::DrawArgs& a_args);
}
