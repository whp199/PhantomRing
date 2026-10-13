#pragma once

// D3D12/DXGI hooks for the renderer, without touching eldenring.exe (Arxan): a throwaway DXGI
// factory, D3D12 device, direct queue and swap chain on a hidden window give us the vtables of the
// classes Elden Ring uses (DXVK's dxgi + vkd3d-proton's d3d12 under Proton); MinHook then detours
// those functions inside the system DLLs (hudhook's technique, docs/research/rendering-options.md §7.1).
//
// Hooked: IDXGISwapChain::Present/Present1 (draw), ResizeBuffers/ResizeBuffers1 (drop back-buffer
// references), IDXGISwapChain3::SetColorSpace1 (HDR output), IDXGIFactory::CreateSwapChain(ForHwnd)
// and ID3D12CommandQueue::ExecuteCommandLists (find the queue that presents).
//
// The presenting queue, in order of preference: the device argument of the swap chain's creation
// (only seen when we hooked before Elden Ring created it), ResizeBuffers1's present queue,
// IDXGISwapChain::GetDevice(ID3D12CommandQueue) (DXVK's swap chain keeps the queue it was created
// with as its "device"), or a DIRECT queue that executes command lists and whose pointer sits inside
// the swap-chain object (hudhook's check_command_queue).

#include "D3D12Util.h"

#include <string>

namespace phantom::er::D3D12Hooks
{
	struct Callbacks
	{
		void (*present)(IDXGISwapChain* a_swapchain, UINT a_flags){ nullptr };  // before the original Present
		void (*resizeBefore)(IDXGISwapChain* a_swapchain){ nullptr };
		void (*resizeAfter)(IDXGISwapChain* a_swapchain, HRESULT a_hr){ nullptr };
		// Every ExecuteCommandLists on any queue, before it runs; may be null. Returning true means the
		// callback submitted the lists itself (through ExecuteOriginal, e.g. split around its own list).
		bool (*execute)(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists){ nullptr };
		// The throwaway device, before it is released: for more device-level hooks (vkd3d-proton's vtables
		// are shared by every device, Elden Ring's included). May be null.
		void (*dummyDevice)(ID3D12Device* a_device){ nullptr };
	};

	// Creates the dummy objects, reads their vtables and enables the detours. False (logged) if any
	// step fails; nothing is left hooked then.
	bool Install(const Callbacks& a_callbacks);
	[[nodiscard]] bool Installed();

	// ExecuteCommandLists without our detour (for a callback that splits a batch).
	void ExecuteOriginal(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists);

	// The direct queue presenting a_swapchain (AddRef'd copy), or null while unknown.
	d3d::ComPtr<ID3D12CommandQueue> PresentQueue(IDXGISwapChain* a_swapchain);
	[[nodiscard]] DXGI_COLOR_SPACE_TYPE ColorSpace();
	std::string                         Describe();
}
