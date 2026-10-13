#pragma once

// Elden Ring's per-view constants on the GPU (er/RenderBind.h finds their descriptors): a tiny compute
// pass, run in our stage C submission with Elden Ring's descriptor heap bound, copies each view's
// constant buffer (64 float4: [3] origin, [24..27] camera-relative view-projection, [28] previous origin,
// [29..32] previous view-projection) into a buffer of ours that our vertex shaders read as a root CBV
// (b4). Snake then uses exactly the matrices Elden Ring rendered the frame with: its TAA jitter, its
// previous frame for velocity, and each shadow cascade's light view.
//
// Trust: every 30th copy of the main view is read back and its origin compared with the camera
// Elden Ring renders from (pers_cam_1); until one matches, and after any mismatch (logged), stage C
// keeps its own camera matrices and draws no shadows.

#include "D3D12Util.h"
#include "Math.h"

#include <cstdint>
#include <string>

namespace phantom::er::RenderView
{
	inline constexpr UINT kSlots = 16;     // 0: the main view, 1..: shadow views
	inline constexpr UINT kFloat4s = 64;   // copied per view

	bool Init(ID3D12Device* a_device);
	// Records the copies (binds Elden Ring's heap and our compute root signature; the caller rebinds its
	// own heap and graphics root signature afterwards). a_camPos: the camera of this frame (validation).
	bool RecordCopies(ID3D12GraphicsCommandList* a_list, ID3D12DescriptorHeap* a_erHeap, const D3D12_GPU_DESCRIPTOR_HANDLE* a_cbs, UINT a_count, const Vec3& a_camPos,
		std::uint64_t a_snapFrame = 0);
	D3D12_GPU_VIRTUAL_ADDRESS Slot(UINT a_index);
	void SetSubmitFence(std::uint64_t a_fence);
	void OnFence(std::uint64_t a_completed);  // Present thread: checks a finished readback
	[[nodiscard]] bool MainTrusted();
	std::string Describe();
}
