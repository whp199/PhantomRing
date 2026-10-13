#pragma once
// Things the GPU may still use, kept until a fence value has completed. Elden Ring's targets that we watch
// (er/RenderDepth) are recorded into our command lists without a reference per submission; when a watched
// target changes (ER recreated it outside ResizeBuffers: a graphics or shadow quality change), the last
// reference to the old one would otherwise drop at Present while our list using it is still in flight.
// Pure (tests/test_render.cpp); not thread-safe: the owner locks.

#include <cstdint>
#include <utility>
#include <vector>

namespace phantom::er
{
	template <class T>
	class Retire
	{
	public:
		// a_fence: the first fence value whose completion proves no submission uses a_item any more.
		void Push(T a_item, std::uint64_t a_fence) { items_.push_back({ std::move(a_item), a_fence }); }
		// Drops what a_completed covers; returns how many.
		std::size_t Collect(std::uint64_t a_completed)
		{
			std::size_t n = 0;
			for (auto it = items_.begin(); it != items_.end();) {
				if (it->second <= a_completed) {
					it = items_.erase(it);
					++n;
				} else {
					++it;
				}
			}
			return n;
		}
		void                      Clear() { items_.clear(); }  // only when the GPU is idle
		[[nodiscard]] std::size_t Size() const { return items_.size(); }

	private:
		std::vector<std::pair<T, std::uint64_t>> items_;
	};
}
