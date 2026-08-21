#include "world/normal_range_allocator.h"
#include <algorithm>

namespace ve {

namespace {
constexpr uint32_t kUnit = 4;

uint32_t round_up(uint32_t value, uint32_t unit) {
	return (value + unit - 1) / unit * unit;
}

uint32_t align_up(uint32_t value, uint32_t alignment) {
	if (alignment == 0) return value;
	return (value + alignment - 1) / alignment * alignment;
}
} // namespace

NormalRangeAllocator::NormalRangeAllocator(uint32_t bytes) : capacity_(bytes) {
	// Round the whole budget down to the four-byte unit; a tail of 0..3 bytes can never
	// satisfy any allocation and would only complicate coalescing.
	capacity_ -= capacity_ % kUnit;
	if (capacity_ > 0) free_.push_back({0, capacity_});
}

NormalAllocation NormalRangeAllocator::allocate(uint32_t bytes, uint32_t alignment) {
	const uint32_t size = round_up(bytes, kUnit);
	if (size == 0 || size > capacity_) return {};
	if (alignment < kUnit) alignment = kUnit;
	alignment = round_up(alignment, kUnit);

	// First fit over the offset-sorted free list: deterministic, and the prefix/suffix
	// splits keep the list sorted without re-sorting.
	for (auto it = free_.begin(); it != free_.end(); ++it) {
		const uint32_t start = align_up(it->offset, alignment);
		// Overflow-safe: start >= it->offset always, so this cannot wrap.
		if (it->size < start - it->offset) continue;
		if (it->size - (start - it->offset) < size) continue;

		const uint32_t prefix = start - it->offset;
		const uint32_t suffix = it->size - prefix - size;
		if (prefix == 0 && suffix == 0) {
			free_.erase(it);
		} else if (prefix > 0 && suffix > 0) {
			// Shrink in place to the prefix fragment; append the suffix after it.
			const FreeBlock tail{start + size, suffix};
			it->size = prefix;
			free_.insert(it + 1, tail);
		} else if (prefix > 0) {
			it->size = prefix;
		} else {
			it->offset = start + size;
			it->size = suffix;
		}

		const uint32_t generation = ++generations_[start];
		live_[start] = {size, generation};
		used_ += size;
		high_water_ = std::max(high_water_, used_);
		return {start, size, generation};
	}
	return {};
}

bool NormalRangeAllocator::release(NormalAllocation allocation) {
	if (!allocation.valid()) return false;
	const auto it = live_.find(allocation.offset);
	if (it == live_.end()) return false;                       // double-free or foreign handle
	if (it->second.first != allocation.size) return false;     // size-mismatched handle
	if (it->second.second != allocation.generation) return false; // stale handle
	live_.erase(it);

	// Insert sorted by offset, then coalesce both neighbours: touching blocks must merge
	// or first-fit fragmentation would grow without bound.
	auto pos = std::lower_bound(free_.begin(), free_.end(), allocation.offset,
			[](const FreeBlock &a, uint32_t offset) { return a.offset < offset; });
	bool absorbed_by_prev = false;
	if (pos != free_.begin()) {
		auto prev = std::prev(pos);
		if (prev->offset + prev->size == allocation.offset) {
			prev->size += allocation.size; // touches predecessor: grow it in place
			absorbed_by_prev = true;
		}
	}
	auto cur = pos;
	if (!absorbed_by_prev)
		cur = free_.insert(pos, {allocation.offset, allocation.size});
	else
		--cur; // the grown predecessor entry
	auto next = std::next(cur);
	if (next != free_.end() && cur->offset + cur->size == next->offset) {
		cur->size += next->size; // touches successor: absorb it
		free_.erase(next);
	}

	used_ -= allocation.size;
	return true;
}

} // namespace ve
