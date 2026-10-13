#pragma once

// The seqlock, SPSC ring and triple-buffer helpers below are adapted from SkyCraft (MIT, Copyright (c) 2026
// chasmlol, https://github.com/chasmlol/SkyCraft: skse/src/Link.cpp); see THIRD-PARTY-NOTICES.md.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include <windows.h>

namespace phantom
{
	// The shared memory both games map. Two ways to reach it:
	//  - a file path (default under Proton: Z:\dev\shm\PhantomRing_v1). Each Proton game has its own
	//    prefix and its own wineserver, so named kernel objects don't cross between them, but a file
	//    on the host's /dev/shm mapped by both is one MAP_SHARED page set: coherent, lock-free.
	//  - a named mapping (Local\PhantomRing_v1), for both games on real Windows (or one wineserver).
	class Mapping
	{
	public:
		Mapping() = default;
		Mapping(const Mapping&) = delete;
		Mapping& operator=(const Mapping&) = delete;
		~Mapping() { Close(); }

		// a_where: "file:<path>" or "name:<kernel object name>". Creates/extends to a_bytes when
		// a_create, else opens what's there (and fails if it's smaller than a_bytes).
		bool Open(const std::wstring& a_where, std::uint64_t a_bytes, bool a_create);
		void Close();

		[[nodiscard]] bool          Valid() const { return base_ != nullptr; }
		[[nodiscard]] std::uint8_t* Base() const { return base_; }
		[[nodiscard]] std::uint64_t Size() const { return size_; }
		[[nodiscard]] const std::wstring& Where() const { return where_; }

	private:
		HANDLE        file_{ INVALID_HANDLE_VALUE };
		HANDLE        mapping_{ nullptr };
		std::uint8_t* base_{ nullptr };
		std::uint64_t size_{ 0 };
		std::wstring  where_;
	};

	// Identity both plugins stamp into the protocol header. Windows pids collide across Proton prefixes,
	// so the header carries unix pids: a_envOverride (if set by the launcher), else getpid(2) through a
	// stub mapped above 0x7001'0000'0000 -- Proton's seccomp filter traps `syscall` issued from PE code
	// below that line (docs/research/proton-ipc-and-launch.md §5.3). On real Windows (no prefixes, one pid
	// namespace) it is the Windows pid. 0 when unknown.
	bool          RunningUnderWine();
	std::uint32_t UnixPid(const wchar_t* a_envOverride = nullptr);

	template <class T>
	std::atomic_ref<T> Atomic(T& a_value)
	{
		return std::atomic_ref<T>(a_value);
	}

	// ---- seqlock (single writer; seq odd while writing) ------------------------------------------
	// T's first member is the u32 sequence; everything after it is the payload.
	template <class T>
	void SeqWrite(T* a_dst, const T& a_src)
	{
		auto       seq = Atomic(*reinterpret_cast<std::uint32_t*>(a_dst));
		const auto s = seq.load(std::memory_order_relaxed);
		seq.store(s + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);
		std::memcpy(reinterpret_cast<std::uint8_t*>(a_dst) + 4, reinterpret_cast<const std::uint8_t*>(&a_src) + 4, sizeof(T) - 4);
		seq.store(s + 2, std::memory_order_release);
	}

	// Returns false if no consistent snapshot was obtained (writer kept writing) or nothing was ever
	// written (seq 0).
	template <class T>
	bool SeqRead(const T* a_src, T& a_out, int a_attempts = 64)
	{
		auto seq = Atomic(*const_cast<std::uint32_t*>(reinterpret_cast<const std::uint32_t*>(a_src)));
		for (int attempt = 0; attempt < a_attempts; ++attempt) {
			const auto s1 = seq.load(std::memory_order_acquire);
			if (s1 == 0) {
				return false;
			}
			if (s1 & 1) {
				YieldProcessor();
				continue;
			}
			std::memcpy(&a_out, a_src, sizeof(T));
			std::atomic_thread_fence(std::memory_order_acquire);
			if (seq.load(std::memory_order_relaxed) == s1) {
				return true;
			}
		}
		return false;
	}

	// ---- fixed-entry SPSC ring: {u64 head @0, u64 tail @0x40, T data[N] @0x80} --------------------
	template <class T, std::uint32_t N>
	struct EntryRing
	{
		static_assert((N & (N - 1)) == 0, "ring size must be a power of two");
		static constexpr std::uint64_t kHeadOff = 0x00;
		static constexpr std::uint64_t kTailOff = 0x40;
		static constexpr std::uint64_t kDataOff = 0x80;
		static constexpr std::uint64_t kBytes = kDataOff + sizeof(T) * N;

		std::uint8_t* base;

		std::uint64_t& HeadRef() const { return *reinterpret_cast<std::uint64_t*>(base + kHeadOff); }
		std::uint64_t& TailRef() const { return *reinterpret_cast<std::uint64_t*>(base + kTailOff); }
		T*             Data() const { return reinterpret_cast<T*>(base + kDataOff); }

		// Producer. Drops (returns false) when the consumer is a full ring behind.
		bool Push(const T& a_value) const
		{
			const auto head = Atomic(HeadRef()).load(std::memory_order_relaxed);
			const auto tail = Atomic(TailRef()).load(std::memory_order_acquire);
			if (head - tail >= N) {
				return false;
			}
			Data()[head & (N - 1)] = a_value;
			Atomic(HeadRef()).store(head + 1, std::memory_order_release);
			return true;
		}

		// Consumer.
		bool Pop(T& a_out) const
		{
			const auto head = Atomic(HeadRef()).load(std::memory_order_acquire);
			auto       tail = Atomic(TailRef()).load(std::memory_order_relaxed);
			if (tail >= head) {
				return false;
			}
			if (head - tail > N) {
				tail = head - N;  // producer lapped us (shouldn't happen: Push refuses)
			}
			a_out = Data()[tail & (N - 1)];
			Atomic(TailRef()).store(tail + 1, std::memory_order_release);
			return true;
		}

		// Consumer: skip everything pending (e.g. the other side restarted).
		void Drain() const { Atomic(TailRef()).store(Atomic(HeadRef()).load(std::memory_order_acquire), std::memory_order_release); }
	};

	// ---- triple buffer of large slots (writer never blocks, reader takes the newest) -------------
	// state bits 0-1: index of the "middle" slot; bit 2: middle holds an unread frame. The writer
	// owns one back slot, renders into it, then xchg(state, back | dirty) and keeps the returned
	// index as its new back slot. The reader xchg(state, front) only when dirty is set and keeps the
	// returned index as its new front. Same scheme as SkyCraft's overlay.
	inline constexpr std::uint32_t kTripleDirty = 1u << 2;

	struct TripleWriter
	{
		std::uint32_t* state;
		std::uint32_t  back{ 1 };
		// Start: middle = 0, writer = 1, reader = 2.
		void Reset() { back = 1; }
		std::uint32_t Publish()
		{
			const auto old = Atomic(*state).exchange(back | kTripleDirty, std::memory_order_acq_rel);
			back = old & 3;
			return back;
		}
	};

	struct TripleReader
	{
		std::uint32_t* state;
		std::uint32_t  front{ 2 };
		void Reset() { front = 2; }
		// True if a newer frame was swapped into `front`.
		bool Acquire()
		{
			if (!(Atomic(*state).load(std::memory_order_acquire) & kTripleDirty)) {
				return false;
			}
			const auto old = Atomic(*state).exchange(front, std::memory_order_acq_rel);
			front = old & 3;
			return true;
		}
	};
}
