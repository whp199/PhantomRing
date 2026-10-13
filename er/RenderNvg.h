#pragma once

// MGSV's night-vision goggles on Elden Ring's frame (GuestState kGuestNvg, protocol v10). MGSV's own goggles only tint its
// hidden picture (and darkened the HUD layer it hands over); Elden Ring's world needs the effect itself. Right before
// Elden Ring's composite -- the same point stage B and the world markers use (er/Renderer.cpp), so Snake, D-Walker and the
// throw arc are in the picture and Elden Ring's HUD stays on top, untouched -- the tone-mapped scene colour is copied and
// drawn back through a full-screen pass: linear luminance x [Render] nvg_gain with a soft highlight roll-off
// (1 - exp(-gain x L)), lifted by nvg_floor, coloured nvg_tint, per-pixel grain that changes every frame, a vignette and
// optional scanlines, blended in by the snapshot's fade (the goggles turning on and off).
//
// Uses the renderer's root signature: the copy in the t6 table (the forward stages' scene-depth slot), the parameters in the
// 16 root constants of b1. Any failure (copy texture, shader, pipeline) turns the effect off for the session, logged once;
// Elden Ring's frame is left alone then.

#include "RenderShared.h"

#include <cstdint>
#include <string>

namespace phantom::er::RenderNvg
{
	struct Target
	{
		ID3D12Resource*             resource{ nullptr };
		D3D12_RESOURCE_STATES       state{ D3D12_RESOURCE_STATE_COMMON };
		D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
		DXGI_FORMAT                 format{ DXGI_FORMAT_UNKNOWN };  // the render target view's (typed)
		bool                        srgbView{ false };              // the view decodes/encodes sRGB itself
		UINT                        width{ 0 }, height{ 0 };
	};

	// Records the pass into a_target (locked by the renderer). a_strength: 0..1 (the fade). False: nothing recorded.
	bool Record(render::Recording& a_rec, const Target& a_target, float a_strength, std::uint64_t a_frame);
	// Swap-chain resize (the renderer waited for the GPU): the copy texture goes.
	void Reset();
	[[nodiscard]] bool Failed();
	[[nodiscard]] bool ShadersCompile();  // tests/test_render.cpp: both entry points compile
	std::string        Describe();
}
