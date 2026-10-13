#pragma once
// One captured draw's geometry turned into a self-contained kRenMesh (DRAW CONTRACT):
//  - only the vertices its index range references (Fox packs a whole model into one buffer set and
//    draws it piece by piece), indices rebased to 0, so the content -- and its key -- is the same in
//    every run and on every machine (co-op: a peer's own MGSV supplies the same assets by key);
//  - only the inputs the vertex shader really reads (ISGN), with Fox's dummy elements dropped: MGSV binds
//    one input layout with every semantic for all shaders and aliases the ones a mesh lacks onto the
//    POSITION bytes (slot 0, offset 0);
//  - interleaved streams that Fox binds twice at different offsets (vb1 = vb3 - 20, stride 32) merged
//    back into one stream; DYNAMIC streams (per-frame vertex colours) are left out.

#include "D3D11Hooks.h"

#include "phantomring_protocol.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace phantom::guest::capture
{
	struct MeshSource
	{
		struct Stream
		{
			ID3D11Buffer* buffer{ nullptr };
			std::uint32_t stride{ 0 };
			std::uint32_t offset{ 0 };
		};
		Stream                         vb[d3d::kVbSlots]{};  // by the game's input slot
		ID3D11Buffer*                  ib{ nullptr };
		std::uint32_t                  ibFormat{ 0 };
		std::uint32_t                  ibOffset{ 0 };
		std::uint32_t                  startIndex{ 0 };
		std::uint32_t                  indexCount{ 0 };
		std::int32_t                   baseVertex{ 0 };
		std::uint32_t                  topology{ 0 };
		std::vector<d3d::InputElement> elements;  // the subset the VS reads, dummies dropped

		std::uint64_t Hash() const;  // pointer-level identity, session-local
	};

	// From the context state at a draw; false when the draw can't be described (no index buffer, ...).
	bool DescribeMesh(const d3d::ContextState& a_state, const d3d::DrawArgs& a_args, MeshSource& a_out);

	struct BuiltMesh
	{
		std::uint64_t             key{ 0 };
		std::vector<std::uint8_t> payload;  // RenMesh + RenInputElement[] + streams + indices
		std::uint32_t             indexCount{ 0 };
		std::uint32_t             vertexCount{ 0 };
		std::uint32_t             bones{ 0 };  // highest BLENDINDICES value + 1 (0: not skinned)
		bool                      weights{ false };
		std::string               layout;   // for the log
	};

	// Needs the bytes of every buffer in a_src (assets::Buffer). nullopt + a_why when it can't be built.
	std::optional<BuiltMesh> BuildMesh(const MeshSource& a_src, const std::shared_ptr<const std::vector<std::uint8_t>> a_vb[d3d::kVbSlots],
		const std::vector<std::uint8_t>& a_ib, std::string& a_why);
}
