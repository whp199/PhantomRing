#pragma once

// Stage C (DESIGN.md §9): Snake's draws injected into Elden Ring's own G-buffer, right after its
// G-buffer lists and before the lighting passes read it, so Elden Ring's deferred lighting lights him
// like the Tarnished: its sun and the shadow mask (computed from the scene depth, which now has Snake),
// its per-area light sets, sky/GI ambient, local lights, SSAO, fog, tone mapping, bloom and exposure.
// A second injection after Elden Ring's object-velocity pass writes Snake's motion vectors so its TAA
// does not smear him.
//
// The encoding is Elden Ring's (decompiled G-buffer and lighting shaders, docs/HOST.md "Stage C"):
//   RT0 R10G10B10A2_UNORM   world normal * 0.5 + 0.5 (+ half a step), a 0
//   RT1 R8G8B8A8_UNORM      metalness, gloss (1 - perceptual roughness)^2, translucency, vertex AO
//   RT2 R8G8B8A8_UNORM_SRGB albedo (linear values into the sRGB view), a = light-set index / 255
//   RT3 R8G8_UNORM          0 for every opaque material
//   RT4 R16G16B16A16_FLOAT  0 for every opaque material
//   depth (reversed Z) and stencil (Elden Ring's per-object class bits, [Render] gbuffer_stencil)
//   velocity R16G16_FLOAT   (NDC now - NDC last frame) * scale + bias
// Every value Elden Ring's characters carry that is not in MGSV's assets (light set, stencil class,
// vertex AO) is a [Render] key, measured on the Tarnished with `render gbuf peek`.

#include "RenderShared.h"

#include <string>
#include <string_view>

namespace phantom::er::Gbuffer
{
	// Renderer Init: our own RTV/DSV descriptors for Elden Ring's targets, the readback buffer.
	bool Init(ID3D12Device* a_device, ID3D12RootSignature* a_root);
	// Can stage C draw this frame? false: a_why says why (the renderer falls back to stage B).
	bool Ready(std::string& a_why);
	// Something to do at the G-buffer / velocity injection points even without draws (a pending peek).
	bool WantsGbufferPoint(const RenderSnapshot* a_snap);
	// The light-set index Snake's G-buffer texels carry ([Render] gbuffer_light_set, or the learned one).
	int LightSet();
	bool WantsVelocityPoint();
	// After Elden Ring's last G-buffer list of the frame: Snake into its G-buffer (a_snap may be null:
	// only a pending peek). False: nothing recorded.
	// a_lists: Elden Ring's lists up to and including its last G-buffer list (where the per-view
	// constants and shadow views are found: er/RenderBind).
	// a_peekHere: a pending `render gbuf peek` is copied here too (else RecordPeek at Elden Ring's last G-buffer list: with
	// Snake drawn before Elden Ring's own G-buffer draws, the peek must come after them to show its characters' texels).
	bool RecordGbuffer(render::Recording& a_rec, const RenderSnapshot* a_snap, UINT a_count, ID3D12CommandList* const* a_lists, bool a_peekHere = true);
	bool PeekPending();
	bool RecordPeek(render::Recording& a_rec);
	// After Elden Ring's object-velocity pass: Snake's motion vectors.
	bool RecordVelocity(render::Recording& a_rec, const RenderSnapshot* a_snap);
	// A submission that recorded a peek completed up to a_fence: read it back (Present thread).
	void OnFence(std::uint64_t a_completed);
	void SetSubmitFence(std::uint64_t a_fence);  // the fence value of the submission just made
	void Reset();  // ResizeBuffers: drop views of Elden Ring's old targets
	std::string Describe();
	// `render gbuf ...`: status | peek [x y] [n] | stats
	std::string Command(std::string_view a_args);
}
