#include "CaptureRing.h"

#include "Link.h"
#include "Log.h"
#include "Mapping.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace phantom::guest::capture::ring
{
	namespace
	{
		using namespace phantom::proto;

		std::atomic<std::uint32_t> g_generation{ 0 };
		std::atomic<std::uint64_t> g_pending{ 0 };
		std::atomic<std::uint64_t> g_lists{ 0 };
		std::uint32_t              g_writerGeneration{ ~0u };  // under the render lock
		std::uint32_t              g_back{ 1 };

		// Sees a new attach generation under the render lock: Link reset the triple state to 0, so our
		// writer restarts on slot 1 (middle 0, reader 2), exactly as the host expects after an attach.
		void Sync()
		{
			const auto gen = link::AttachGeneration();
			if (gen != g_writerGeneration) {
				g_writerGeneration = gen;
				g_back = 1;
			}
			g_generation.store(gen, std::memory_order_release);
		}
	}

	Result Push(proto::RenType a_type, std::initializer_list<Part> a_parts, std::uint32_t& a_generation)
	{
		std::size_t payload = 0;
		for (const auto& p : a_parts) {
			payload += p.bytes;
		}
		const std::uint64_t msg = (sizeof(RenMsgHeader) + payload + 7) & ~std::uint64_t(7);
		if (msg > kRenRingDataBytes / 2 || payload > 0xFFFFFFFFull) {
			return Result::kTooBig;
		}
		std::unique_lock<std::mutex> lock;
		auto*                        base = link::LockRender(lock);
		if (!base) {
			return Result::kDetached;
		}
		Sync();
		auto*      ring = base + kOffRenderRing;
		auto&      headRef = *reinterpret_cast<std::uint64_t*>(ring + kRenRingHeadOff);
		auto&      tailRef = *reinterpret_cast<std::uint64_t*>(ring + kRenRingTailOff);
		const auto head = Atomic(headRef).load(std::memory_order_relaxed);
		const auto tail = Atomic(tailRef).load(std::memory_order_acquire);
		auto       pos = head % kRenRingDataBytes;
		const std::uint64_t skip = pos + msg > kRenRingDataBytes ? kRenRingDataBytes - pos : 0;
		g_pending.store(head - tail, std::memory_order_relaxed);
		if (head + skip + msg - tail > kRenRingDataBytes) {
			return Result::kFull;
		}
		auto* data = ring + kRenRingDataOff;
		if (skip) {
			// The rest of the ring is one pad message (skip is a multiple of 8 and >= 8: positions are 8-aligned).
			const RenMsgHeader pad{ kRenPad, static_cast<std::uint32_t>(skip - sizeof(RenMsgHeader)) };
			std::memcpy(data + pos, &pad, sizeof(pad));
			pos = 0;
		}
		std::size_t at = pos + sizeof(RenMsgHeader);
		for (const auto& p : a_parts) {
			if (p.bytes) {
				std::memcpy(data + at, p.data, p.bytes);
			}
			at += p.bytes;
		}
		const RenMsgHeader hdr{ a_type, static_cast<std::uint32_t>(payload) };
		std::memcpy(data + pos, &hdr, sizeof(hdr));
		Atomic(headRef).store(head + skip + msg, std::memory_order_release);
		g_pending.store(head + skip + msg - tail, std::memory_order_relaxed);
		a_generation = g_writerGeneration;
		return Result::kOk;
	}

	bool PublishDraws(const void* a_list, std::size_t a_bytes, std::uint64_t a_frame)
	{
		if (a_bytes > kDrawSlotBytes) {
			return false;
		}
		std::unique_lock<std::mutex> lock;
		auto*                        base = link::LockRender(lock);
		if (!base) {
			return false;
		}
		Sync();
		auto* ctl = reinterpret_cast<DrawCtl*>(base + kOffDrawCtl);
		auto* hdrs = reinterpret_cast<DrawSlotHdr*>(base + kOffDrawCtl + sizeof(DrawCtl));
		std::memcpy(base + kOffDrawSlots + g_back * kDrawSlotBytes, a_list, a_bytes);
		hdrs[g_back].frame = a_frame;
		hdrs[g_back].bytes = static_cast<std::uint32_t>(a_bytes);
		const auto old = Atomic(ctl->state).exchange(g_back | kTripleDirty, std::memory_order_acq_rel);
		g_back = old & 3;
		Atomic(ctl->framesPublished).fetch_add(1, std::memory_order_relaxed);
		g_lists.fetch_add(1, std::memory_order_relaxed);
		return true;
	}

	std::uint64_t Pending() { return g_pending.load(std::memory_order_relaxed); }
	std::uint32_t Generation() { return g_generation.load(std::memory_order_acquire); }
	std::uint64_t ListsPublished() { return g_lists.load(std::memory_order_relaxed); }
}
