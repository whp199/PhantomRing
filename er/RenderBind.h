#pragma once

// Where Elden Ring's per-view constants live, per command list: its renderer is bindless (one root
// signature for everything, descriptor tables per root parameter, vkd3d-proton turns each into a push
// constant `registers[n]`), and every mesh vertex shader reads its view -- origin at [3], camera-
// relative view-projection rows at [24..27], the previous frame's origin and rows at [28..32] -- from
// the 5th CBV of root parameter 1's table (decompiled G-buffer, shadow and velocity vertex shaders,
// docs/HOST.md "Stage C"). That is the main camera in the G-buffer pass (TAA jitter included) and each
// cascade's light view in the shadow atlas passes.
//
// Hooks (installed with the frame watch, forwarding to the frame probe while it records):
// ID3D12GraphicsCommandList::SetDescriptorHeaps (Elden Ring's CBV/SRV/UAV heap), ::RSSetViewports (which
// cascade of the atlas), ::SetGraphicsRootDescriptorTable (root parameter 1). The pass a list is in
// comes from er/RenderDepth's OMSetRenderTargets watch.

#include "D3D12Util.h"

#include <string>
#include <vector>

namespace phantom::er::RenderBind
{
	enum class Pass : std::uint8_t
	{
		kOther,
		kGbuffer,
		kShadow,  // a pass into a shadow atlas (R16 depth, no colour target)
	};

	struct View
	{
		D3D12_GPU_DESCRIPTOR_HANDLE cb{};          // the view's constant buffer descriptor in Elden Ring's heap
		D3D12_VIEWPORT              vp{};
		D3D12_CPU_DESCRIPTOR_HANDLE dsv{};         // Elden Ring's DSV of the pass
		ID3D12Resource*             depth{ nullptr };  // the atlas
	};

	bool Install(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_ours);
	// From er/RenderDepth: a list now renders a_pass (Reset: kOther).
	void OnTargets(ID3D12GraphicsCommandList* a_list, Pass a_pass, SIZE_T a_dsv, ID3D12Resource* a_depth);
	void OnReset(ID3D12GraphicsCommandList* a_list);
	// Of these lists (a prefix of a batch, in submission order): the G-buffer pass's view, the shadow
	// views (one per atlas viewport, the latest), and Elden Ring's CBV/SRV/UAV heap. False: no G-buffer view.
	bool Collect(UINT a_count, ID3D12CommandList* const* a_lists, View& a_gbuffer, std::vector<View>& a_shadows, ID3D12DescriptorHeap*& a_heap);
	std::string Describe();
}
