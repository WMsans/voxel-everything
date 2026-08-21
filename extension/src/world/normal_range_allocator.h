#pragma once
#include <cstdint>
#include <map>
#include <vector>

namespace ve {

// A handle to one live span inside a NormalRangeAllocator. `generation` disambiguates a
// span that was freed and handed out again from the stale handle a caller may still be
// holding: releasing a handle whose generation does not match the live one fails without
// touching state, which is what keeps a late release_volume() from punching a hole under
// a newer upload.
struct NormalAllocation {
	uint32_t offset = 0;
	uint32_t size = 0;
	uint32_t generation = 0;

	bool valid() const { return size != 0 && generation != 0; }
};

// Fixed-capacity first-fit range allocator over a flat byte budget. Pure C++: no Godot
// types, no allocation after construction besides the container nodes.
//
// Free blocks are kept sorted by offset so first-fit is deterministic and adjacent blocks
// coalesce in O(1) neighbours on release. Sizes are rounded up to four bytes because every
// consumer stores uint16 samples and every GPU buffer_update wants aligned ranges.
class NormalRangeAllocator {
public:
	explicit NormalRangeAllocator(uint32_t bytes = 0);

	NormalAllocation allocate(uint32_t bytes, uint32_t alignment = 4);
	bool release(NormalAllocation allocation);

	uint32_t used_bytes() const { return used_; }
	uint32_t high_water_bytes() const { return high_water_; }

private:
	struct FreeBlock {
		uint32_t offset;
		uint32_t size;
	};

	uint32_t capacity_ = 0;
	uint32_t used_ = 0;
	uint32_t high_water_ = 0;
	std::vector<FreeBlock> free_; // sorted by offset, never overlapping or touching
	// Live spans keyed by offset -> {size, generation}. An offset can only be live once,
	// so the key is unambiguous.
	std::map<uint32_t, std::pair<uint32_t, uint32_t>> live_;
	// Last generation handed out per offset. Offsets keep their history across frees so
	// every reuse bumps the generation a stale handle cannot match.
	std::map<uint32_t, uint32_t> generations_;
};

} // namespace ve
