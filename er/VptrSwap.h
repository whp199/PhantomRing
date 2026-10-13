#pragma once

// Instance-vtable swap: one object gets a private copy of its vtable with one slot replaced. Nothing in
// eldenring.exe's code or .rdata is written (Arxan never sees it) and every other object of the class
// keeps the original. The copy starts at vtable[-1] (the RTTI CompleteObjectLocator), so RTTI on the
// swapped object still names its class. The copy is static storage: an object the game frees (or one
// we lost track of) can keep pointing at it forever, and every slot but ours still is the game's.

#include "Mem.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace phantom::er
{
	template <std::size_t kSlots>
	class VptrSwap
	{
	public:
		// a_object's vptr must be a_vtable and its slot a_slot must be a_expectedFn; then the object is
		// switched to the copy with a_detour in that slot. Returns false (and changes nothing) otherwise.
		bool Install(std::uint8_t* a_object, void* a_vtable, std::size_t a_slot, void* a_expectedFn, void* a_detour)
		{
			if (!a_object || !a_vtable || a_slot >= kSlots) {
				return false;
			}
			const auto vptr = SafeRead<void*>(a_object);
			if (!vptr) {
				return false;
			}
			if (*vptr == Fake() && original_ == a_vtable) {
				return true;  // already ours
			}
			if (*vptr != a_vtable) {
				return false;
			}
			void* copy[kSlots + 1];
			if (!SafeReadBytes(static_cast<std::uint8_t*>(a_vtable) - sizeof(void*), copy, sizeof(copy)) || copy[1 + a_slot] != a_expectedFn) {
				return false;
			}
			for (std::size_t i = 0; i <= kSlots; ++i) {
				table_[i] = copy[i];
			}
			table_[1 + a_slot] = a_detour;
			original_ = a_vtable;
			slot_ = a_slot;
			originalFn_ = a_expectedFn;
			std::atomic_ref<void*>(*reinterpret_cast<void**>(a_object)).store(Fake(), std::memory_order_release);
			return true;
		}

		// Back to the game's vtable, only if a_object still uses our copy.
		bool Restore(std::uint8_t* a_object)
		{
			if (!a_object || !original_) {
				return false;
			}
			const auto vptr = SafeRead<void*>(a_object);
			if (!vptr || *vptr != Fake()) {
				return false;
			}
			void* expected = Fake();
			return std::atomic_ref<void*>(*reinterpret_cast<void**>(a_object)).compare_exchange_strong(expected, original_, std::memory_order_acq_rel);
		}

		[[nodiscard]] bool  Owns(const std::uint8_t* a_object) const
		{
			const auto vptr = a_object ? SafeRead<void*>(a_object) : std::nullopt;
			return vptr && *vptr == Fake();
		}
		[[nodiscard]] void* Fake() const { return const_cast<void**>(&table_[1]); }
		[[nodiscard]] void* OriginalFn() const { return originalFn_; }

	private:
		void*       table_[kSlots + 1]{};
		void*       original_{ nullptr };
		void*       originalFn_{ nullptr };
		std::size_t slot_{ 0 };
	};
}
