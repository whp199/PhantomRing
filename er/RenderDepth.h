#pragma once

// Watching Elden Ring's frame from its command lists (re-backlog E5): the scene depth (so Snake is hidden
// behind Elden Ring's world), the G-buffer MRTs and the velocity target (stage C: Snake drawn into ER's
// own G-buffer so its deferred lighting lights him), the post-tonemap scene colour target and the
// composite pass (stage B: drawn before the HUD goes on top).
//
// Elden Ring creates its screen-size targets again on every ResizeBuffers (it does one right after the
// title screen, after our hooks are live), so watching ER's device and command lists is enough:
//  - ID3D12Device::CreateDepthStencilView: DSV descriptor -> resource (+ its desc); RTVs come from
//    er/RenderProbe's CreateRenderTargetView watch;
//  - ID3D12GraphicsCommandList::ClearDepthStencilView / ClearRenderTargetView: which targets are
//    cleared each frame, and to what depth (0 = reversed Z);
//  - ::OMSetRenderTargets: how often each DSV is bound, which lists render into a back buffer (the
//    composite), the G-buffer (5 MRTs R10G10B10A2 / RGBA8 / RGBA8_SRGB / R8G8 / RGBA16F with the scene
//    depth), the velocity target (R16G16_FLOAT with the scene depth), the post-tonemap colour target;
//  - ::ResourceBarrier + ID3D12CommandQueue::ExecuteCommandLists: the watched resources' states as of
//    the last executed barrier (per list, applied in submission order).
// Live, Elden Ring 1.17.1 at 4K SDR: the scene depth is R32G8X24_TYPELESS cleared to 0, bound ~200x per
// frame; G-buffer lists 12-29 of the 37-list batch after the shadow cascades, the velocity pass in the
// forward batch (`render probe shaders`, docs/HOST.md "Frame structure").
// Everything is in system DLLs (vkd3d-proton); nothing in eldenring.exe is touched.

#include "D3D12Util.h"

#include <cstdint>
#include <string>

namespace phantom::er::RenderDepth
{
	struct Target
	{
		ID3D12Resource*       resource{ nullptr };
		DXGI_FORMAT           format{ DXGI_FORMAT_UNKNOWN };     // resource format
		DXGI_FORMAT           srvFormat{ DXGI_FORMAT_UNKNOWN };
		DXGI_FORMAT           rtvFormat{ DXGI_FORMAT_UNKNOWN };  // how Elden Ring binds it as a render target
		UINT                  width{ 0 }, height{ 0 };
		D3D12_RESOURCE_STATES state{ D3D12_RESOURCE_STATE_COMMON };
		float                 clearDepth{ 0 };  // what Elden Ring clears it to (0: reversed Z)
		std::uint32_t         generation{ 0 };  // changes whenever the watched resource does (a reused address too)
	};

	inline constexpr int kGbufferTargets = 5;

	// What a command list of Elden Ring's did (flags, per list, since its last Reset).
	enum ListKind : std::uint8_t
	{
		kListComposite = 1,  // renders into a back buffer
		kListGbuffer = 2,    // binds the 5 G-buffer MRTs with the scene depth
		kListVelocity = 4,   // binds the full-size R16G16_FLOAT velocity target with the scene depth
		kListDepthClear = 8,  // clears the scene depth in full (the start of Elden Ring's G-buffer: nothing drawn into it yet)
		kListGbufAfterClear = 16,  // binds the G-buffer after that clear in the same list (its draws may follow in this list)
	};

	// After the renderer knows ER's device: hooks (a_list = any command list of that device).
	bool Install(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list);
	void SetBackBuffers(ID3D12Resource* const* a_buffers, UINT a_count);
	// From the ExecuteCommandLists detour (every queue), before the lists run: applies their states.
	void OnExecute(UINT a_count, ID3D12CommandList* const* a_lists);
	// The first list of a batch that renders into a back buffer (Elden Ring's composite), or -1.
	int CompositeIndex(UINT a_count, ID3D12CommandList* const* a_lists);
	// The last list of a batch with that kind, or -1.
	int LastIndex(ListKind a_kind, UINT a_count, ID3D12CommandList* const* a_lists);
	// Whether that list has that kind (since its last Reset).
	bool HasKind(ID3D12CommandList* a_list, ListKind a_kind);
	// The a_nth (1-based) list of a batch with that kind (the last one if there are fewer), or -1.
	int NthIndex(ListKind a_kind, int a_nth, UINT a_count, ID3D12CommandList* const* a_lists);
	// Once per Present (frame boundary): picks the scene depth, colour, G-buffer and velocity targets.
	void EndFrame(UINT a_bbWidth, UINT a_bbHeight);
	// The scene depth / post-tonemap scene colour, with known states, or false.
	bool Current(Target& a_out);
	bool SceneColor(Target& a_out);
	// The 5 G-buffer MRTs as Elden Ring bound them last frame, states known; false (a_why says why) if
	// the frame did not have exactly the structure stage C was built for.
	bool Gbuffer(Target (&a_out)[kGbufferTargets], std::string* a_why = nullptr);
	bool Velocity(Target& a_out);
	// A shadow atlas Elden Ring rendered recently (R16 depth), with its state; false if unknown. An atlas not bound
	// for kAtlasIdleFrames is let go (its slot can take a recreated or another area's atlas).
	inline constexpr std::uint64_t kAtlasIdleFrames = 60;
	bool Atlas(ID3D12Resource* a_res, Target& a_out);
	// Its Target::generation while watched, else 0 (a DSV made for an older generation must be made again).
	std::uint32_t AtlasGeneration(ID3D12Resource* a_res);
	// Elden Ring cleared all of it (ClearDepthStencilView without rects) since the last Present.
	bool FullyClearedThisFrame(ID3D12Resource* a_res);
	// From the renderer: the fence value of its last submission, and what has completed. A watched target that
	// changes is kept alive until a submission after the change has completed (er/Retire.h).
	void SetSubmitFence(std::uint64_t a_fence);
	void OnFence(std::uint64_t a_completed);
	// The scene depth's stencil plane (subresource 1) state, when a barrier moved it; false: unknown
	// (then it is in the same state as the depth plane).
	bool StencilPlane(D3D12_RESOURCE_STATES& a_state);
	// The resource behind a DSV descriptor Elden Ring created (null if unknown); for the frame probe.
	ID3D12Resource* DsvResource(SIZE_T a_handle, std::uint32_t& a_w, std::uint32_t& a_h, int& a_format);
	// Before ResizeBuffers (GPU idle): drop our references so Elden Ring can free its old targets.
	void Reset();
	std::string Describe();
}
