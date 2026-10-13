#pragma once

// The guest's meshes and textures on Elden Ring's GPU (DRAW CONTRACT, protocol/phantomring_protocol.h
// render ring). A worker thread consumes the render ring (kRenMesh / kRenTexture / kRenForget /
// kRenClearAll), validates each message, creates the DEFAULT-heap resource plus an UPLOAD staging copy
// of its bytes, and queues the copy. The Present thread records the queued copies at the start of its
// own command list (bounded per frame), so a resource is used only after its copy, in the same list.
// Everything is keyed by the guest's 64-bit content hash; objects are shared_ptr-owned, and every
// frame keeps what it used alive until its fence passes, so kRenForget/kRenClearAll never free memory
// the GPU is still reading.

#include "D3D12Util.h"
#include "phantomring_protocol.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace phantom::er::RenderAssets
{
	struct Mesh
	{
		std::uint64_t                         key{ 0 };
		d3d::ComPtr<ID3D12Resource>           buffer;  // vertex streams then indices
		std::uint32_t                         streams{ 1 };
		std::uint32_t                         streamOffset[4]{}, streamStride[4]{}, streamBytes[4]{};
		std::uint32_t                         indexOffset{ 0 }, indexBytes{ 0 }, indexCount{ 0 };
		DXGI_FORMAT                           indexFormat{ DXGI_FORMAT_R16_UINT };
		D3D_PRIMITIVE_TOPOLOGY                topology{ D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST };
		std::vector<proto::RenInputElement>   elements;
		std::uint64_t                         layoutHash{ 0 };
		bool                                  hasBlendIndices{ false };
		int                                   winding{ 0 };  // er/RenderMesh.h: +1 outward, -1 inward (MGSV), 0 unknown
		bool                                  hasBounds{ false };  // bind-space POSITION box (er/RenderMesh.h Bounds)
		float                                 boundsMin[3]{}, boundsMax[3]{};
		std::uint32_t                         uploadBytes{ 0 };
		std::atomic<bool>                     ready{ false };
	};

	struct Texture
	{
		std::uint64_t               key{ 0 };
		d3d::ComPtr<ID3D12Resource> resource;
		DXGI_FORMAT                 format{ DXGI_FORMAT_UNKNOWN };
		std::uint32_t               width{ 0 }, height{ 0 }, mips{ 0 }, slices{ 0 };
		std::int32_t                descriptor{ -1 };  // slot in the CPU SRV heap
		std::uint32_t               uploadBytes{ 0 };
		std::atomic<bool>           ready{ false };
		~Texture();
	};

	// Starts the worker on Elden Ring's device (first Present). False: logged, nothing started.
	bool Start(ID3D12Device* a_device);
	void Stop();

	// Ready (uploaded) objects only; null otherwise.
	std::shared_ptr<Mesh>    FindMesh(std::uint64_t a_key);
	std::shared_ptr<Texture> FindTexture(std::uint64_t a_key);
	D3D12_CPU_DESCRIPTOR_HANDLE Srv(const Texture& a_texture);

	[[nodiscard]] bool HasPendingUploads();
	// Present thread: records up to a_budgetBytes of queued copies; staging buffers go to a_keep.
	void RecordUploads(ID3D12GraphicsCommandList* a_list, std::vector<std::shared_ptr<void>>& a_keep, std::uint64_t a_budgetBytes);

	std::string Describe();
}
