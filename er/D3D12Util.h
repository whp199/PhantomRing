#pragma once

// Small D3D12/DXGI helpers shared by the renderer files: COM pointers, DXGI format facts (block size,
// component class, typeless -> typed), and the vtable indices we hook (computed from the mingw-w64
// d3d12.h/dxgi1_4.h headers, docs/research/rendering-options.md §7.1).

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>

namespace phantom::er::d3d
{
	using Microsoft::WRL::ComPtr;

	namespace vt
	{
		// IDXGIFactory2
		inline constexpr std::size_t kFactoryCreateSwapChain = 10;
		inline constexpr std::size_t kFactoryCreateSwapChainForHwnd = 15;
		// IDXGISwapChain / 1 / 3
		inline constexpr std::size_t kSwapPresent = 8;
		inline constexpr std::size_t kSwapResizeBuffers = 13;
		inline constexpr std::size_t kSwapPresent1 = 22;
		inline constexpr std::size_t kSwapSetColorSpace1 = 38;
		inline constexpr std::size_t kSwapResizeBuffers1 = 39;
		// ID3D12CommandQueue
		inline constexpr std::size_t kQueueExecuteCommandLists = 10;
	}

	// What the vertex shader input / sampler sees for a DXGI format.
	enum class Kind : std::uint8_t
	{
		kUnknown,
		kFloat,  // FLOAT/UNORM/SNORM: float in the shader
		kUint,
		kSint,
	};

	struct FormatInfo
	{
		Kind          kind{ Kind::kUnknown };
		std::uint8_t  comps{ 0 };
		bool          unorm{ false };   // UNORM (normals stored as UNORM need *2-1)
		bool          block{ false };   // BCn: bytes is per 4x4 block
		bool          srgb{ false };
		std::uint32_t bytes{ 0 };       // per element / pixel / block
		float         unormMax{ 0 };    // 255 / 65535 / 1023 for UNORM integer formats (BLENDINDICES stored as UNORM)
	};

	FormatInfo  Info(DXGI_FORMAT a_format);
	// Typeless -> the plain typed format a texture SRV can use (BC1_TYPELESS -> BC1_UNORM, ...).
	DXGI_FORMAT Typed(DXGI_FORMAT a_format);
	// For a render target view of a swap-chain buffer.
	DXGI_FORMAT RtvFormat(DXGI_FORMAT a_format);
	const char* Name(DXGI_FORMAT a_format);
	// Bytes of one tightly packed row / number of rows of a w x h mip (block rows for BCn).
	void        MipLayout(DXGI_FORMAT a_format, std::uint32_t a_w, std::uint32_t a_h, std::uint32_t& a_rowBytes, std::uint32_t& a_rows);

	std::string Hr(HRESULT a_hr);  // "0x887A0005 (DXGI_ERROR_DEVICE_REMOVED)"

	inline D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_res, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after,
		UINT a_sub = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES)
	{
		D3D12_RESOURCE_BARRIER b{};
		b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		b.Transition.pResource = a_res;
		b.Transition.StateBefore = a_before;
		b.Transition.StateAfter = a_after;
		b.Transition.Subresource = a_sub;
		return b;
	}

	inline D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE a_type)
	{
		D3D12_HEAP_PROPERTIES p{};
		p.Type = a_type;
		p.CreationNodeMask = 1;
		p.VisibleNodeMask = 1;
		return p;
	}

	inline D3D12_RESOURCE_DESC BufferDesc(std::uint64_t a_bytes)
	{
		D3D12_RESOURCE_DESC d{};
		d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		d.Width = a_bytes;
		d.Height = 1;
		d.DepthOrArraySize = 1;
		d.MipLevels = 1;
		d.Format = DXGI_FORMAT_UNKNOWN;
		d.SampleDesc.Count = 1;
		d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		return d;
	}

	// Waits until a_fence reaches a_value, at most a_ms. The event is auto-reset and a timed-out wait
	// leaves its SetEventOnCompletion registration pending: that registration fires later with nobody
	// waiting, so a later wait can wake at once on a stale signal. Hence the event is never trusted
	// alone: re-check the fence, re-register, and wait out the rest of the deadline. A template so
	// tests can drive it with a fake fence.
	template <class Fence>
	bool WaitFence(Fence* a_fence, HANDLE a_event, std::uint64_t a_value, DWORD a_ms)
	{
		const ULONGLONG until = ::GetTickCount64() + a_ms;
		while (a_fence->GetCompletedValue() < a_value) {
			const ULONGLONG now = ::GetTickCount64();
			if (now >= until || FAILED(a_fence->SetEventOnCompletion(a_value, a_event))) {
				return false;
			}
			::WaitForSingleObject(a_event, static_cast<DWORD>(until - now));
		}
		return true;
	}

	inline std::uint64_t Align(std::uint64_t a_value, std::uint64_t a_to) { return (a_value + a_to - 1) & ~(a_to - 1); }
}
