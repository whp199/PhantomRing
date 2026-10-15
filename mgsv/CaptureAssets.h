#pragma once
// GPU -> CPU copies of MGSV's own vertex/index buffers and textures, made once per resource.
//
// Snake's buffers are IMMUTABLE and his textures DEFAULT-usage, so the CPU never sees their bytes:
// we copy them into staging resources on the immediate context at Present (the one moment that
// context is ours on the presenting thread), map them a frame or two later without waiting
// (D3D11_MAP_FLAG_DO_NOT_WAIT), and keep the bytes. A few copies per frame, within a byte budget, so
// MGSV keeps its frame rate while a new outfit/weapon streams in. Every resource we copy is AddRef'd
// for as long as we keep it, so its address can't be reused by another resource under our cache key.

#include "CaptureKeys.h"

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace phantom::guest::capture::assets
{
	struct TextureBytes
	{
		D3D11_TEXTURE2D_DESC      desc{};
		std::vector<std::uint8_t> bytes;  // every mip of slice 0, mip-major, rows tightly packed (BC blocks as stored)
	};

	// Any thread. The bytes once copied (nullptr until then; the first call queues the copy).
	std::shared_ptr<const std::vector<std::uint8_t>> Buffer(ID3D11Buffer* a_buffer);
	std::shared_ptr<const TextureBytes>              Texture(ID3D11Texture2D* a_texture);
	bool                                             Failed(ID3D11Resource* a_resource);
	// Any thread. Which copy of a_resource this is: a number never reused, given when the resource is first seen, 0 when it isn't
	// known (never asked for, or evicted). Callers that key anything by the game's pointer compare it each use: once the copy
	// is evicted our reference is gone and MGSV may create another resource at the same address (Capture.cpp). a_touch keeps
	// the copy from being evicted while it is in use.
	std::uint64_t                                    Serial(ID3D11Resource* a_resource, bool a_touch);
	using keys::Stale;  // CaptureKeys.h

	// Present thread (owns the immediate context): finish copies the GPU completed, start new ones.
	void Pump(ID3D11Device* a_device, ID3D11DeviceContext* a_immediate);

	// Releases resources nobody asked for (Buffer/Texture/Serial with a_touch) in a minute (Present thread, cheap).
	void Evict(std::uint64_t a_present);

	std::string Describe();

	// Bytes of one row of 4x4 blocks (BC) or pixels, rows per mip, for a DXGI format; 0 if unsupported.
	std::uint32_t RowBytes(std::uint32_t a_format, std::uint32_t a_width);
	std::uint32_t Rows(std::uint32_t a_format, std::uint32_t a_height);
}
