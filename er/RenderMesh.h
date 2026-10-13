#pragma once

// Facts about a guest mesh computed once when it arrives (er/RenderAssets): its triangle winding relative
// to its own vertex normals. MGSV's meshes are wound so that cross(b - a, c - a) points INTO the surface
// (against the vertex normal) on every recorded mesh (live recordings, 2026-10-12), while the fake
// guest's test model is wound outward; the front-face setting that suits one turns the other inside out
// (Snake's head showed its inside, and lit by Elden Ring his flipped normals faced away from the light).
// Pure code, unit-tested.

#include "phantomring_protocol.h"

#include <cstdint>
#include <span>

namespace phantom::er::RenderMesh
{
	// +1: cross(b - a, c - a) agrees with the vertex normals (outward winding), -1: against them
	// (inward), 0: unknown (no usable POSITION/NORMAL, or no clear majority).
	// a_streams[s] / a_streamBytes[s] / a_strides[s]: the vertex streams in slot order.
	int Winding(std::span<const proto::RenInputElement> a_elements, const std::uint8_t* const* a_streams, const std::uint32_t* a_streamBytes,
		const std::uint32_t* a_strides, std::uint32_t a_streamCount, const std::uint8_t* a_indices, std::uint32_t a_indexBytes, bool a_index32, bool a_strip);

	// The bind-space bounding box of the mesh's POSITIONs (every vertex); false without a decodable POSITION. The
	// renderer moves its centre with each draw's matrices to find where a buddy's body is: the translations alone sit
	// at its feet (rigid parts' origins, bind-pose offsets), a metre below the camera line it may block.
	bool Bounds(std::span<const proto::RenInputElement> a_elements, const std::uint8_t* const* a_streams, const std::uint32_t* a_streamBytes,
		const std::uint32_t* a_strides, std::uint32_t a_streamCount, float (&a_min)[3], float (&a_max)[3]);

	// One vertex attribute as floats (FLOAT32, FLOAT16, UNORM/SNORM 8/16); false for other formats.
	bool Decode(std::uint32_t a_format, const std::uint8_t* a_p, float (&a_out)[4]);
}
