#pragma once
// The producer ends of the two phase-2 channels (protocol/phantomring_protocol.h, docs/TOOLS.md):
// the render byte ring (kRenMesh / kRenTexture / kRenForget, contiguous messages, kRenPad at the wrap)
// and the draw-list triple buffer. Both are only written under link::LockRender, so a host re-attach
// (Link.cpp: kRenClearAll + triple reset) never interleaves with us.

#include "phantomring_protocol.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>

namespace phantom::guest::capture::ring
{
	struct Part
	{
		const void* data;
		std::size_t bytes;
	};

	enum class Result
	{
		kOk,
		kFull,      // the host is behind: try again later
		kDetached,  // no host session: nothing was written
		kTooBig,    // larger than the whole ring
	};

	// Appends one message made of a_parts back to back. a_generation receives the link attach generation
	// the message went out in (the host dropped everything from older generations).
	Result Push(proto::RenType a_type, std::initializer_list<Part> a_parts, std::uint32_t& a_generation);

	// Writes one DrawList (header + records + matrices, already laid out in a_list) into the back slot and
	// swaps it in. False while detached or when it doesn't fit a slot.
	bool PublishDraws(const void* a_list, std::size_t a_bytes, std::uint64_t a_frame);

	// Bytes the host has not consumed yet (0 while detached), and the generation seen last.
	std::uint64_t Pending();
	std::uint32_t Generation();
	std::uint64_t ListsPublished();
}
