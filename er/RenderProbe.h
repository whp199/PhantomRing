#pragma once

// Stage B groundwork (rendering-options.md §7.3): a one-shot dump of Elden Ring's frame structure, to
// find where Snake could be drawn before Elden Ring's post-processing and UI.
//
// `pr er "render probe"` records two frames: every ExecuteCommandLists batch (queue, list count) and,
// per command list in submission order, its passes -- each OMSetRenderTargets with the bound targets
// (format, size, back buffer?, the main scene depth?) and the draws/dispatches/indirect calls and
// render-target clears recorded under it -- then writes the second frame to the log. Render-target
// views are mapped to resources from their creation (ID3D12Device::CreateRenderTargetView, watched from
// the renderer's start; Elden Ring recreates its targets on the ResizeBuffers after the title screen).
// The draw/dispatch hooks only count while a probe runs.

#include "D3D12Util.h"

#include <string>

namespace phantom::er::RenderProbe
{
	bool Install(ID3D12Device* a_device, ID3D12GraphicsCommandList* a_list);  // renderer Init
	void Request(bool a_shaders = false);  // console; a_shaders: name each pass's pipelines (RenderPso)
	// Called by the other detours / the renderer:
	void OnSetTargets(ID3D12GraphicsCommandList* a_list, UINT a_n, const D3D12_CPU_DESCRIPTOR_HANDLE* a_rtvs, BOOL a_single, const D3D12_CPU_DESCRIPTOR_HANDLE* a_dsv);
	void OnReset(ID3D12GraphicsCommandList* a_list, ID3D12PipelineState* a_initial = nullptr);
	void OnClearRtv(ID3D12GraphicsCommandList* a_list);
	// From er/RenderBind's detours (they own those hooks).
	void OnViewports(ID3D12GraphicsCommandList* a_list, UINT a_n, const D3D12_VIEWPORT* a_vp);
	void OnHeaps(ID3D12GraphicsCommandList* a_list, UINT a_n, ID3D12DescriptorHeap* const* a_heaps);
	void OnRootTable(ID3D12GraphicsCommandList* a_list, UINT a_param, D3D12_GPU_DESCRIPTOR_HANDLE a_h);
	// The resource behind an RTV descriptor Elden Ring created (false if unknown).
	bool RtvInfo(SIZE_T a_handle, ID3D12Resource*& a_res, DXGI_FORMAT& a_format, UINT& a_w, UINT& a_h);
	void OnExecute(ID3D12CommandQueue* a_queue, UINT a_count, ID3D12CommandList* const* a_lists);
	void OnPresent(IDXGISwapChain* a_swap, ID3D12Resource* const* a_backBuffers, UINT a_count, ID3D12Resource* a_mainDepth, D3D12_CPU_DESCRIPTOR_HANDLE a_mainDsv);
	std::string Describe();
}
