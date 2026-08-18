#pragma once
#include <vector>

namespace ve {

// Quads -> pages, rounded up and clamped to kLodMaxPagesPerChunk.
int lod_pages_for_quads(int quads);

// The geometry arena's page allocator. Draw granularity is the page (spec section 3.3), so a
// chunk's pages need not be contiguous and there is no suballocator, no fragmentation, and
// no compaction pass.
class LodArena {
public:
	explicit LodArena(int page_count);

	// All-or-nothing: an allocation that cannot be fully funded takes nothing and leaves
	// `out` empty (M3 errata 5 -- a partially funded load is worse than a refused one).
	bool alloc(int pages, std::vector<int> *out);
	void release(const std::vector<int> &pages);

	int free_pages() const { return static_cast<int>(free_.size()); }
	int used_pages() const { return capacity_ - free_pages(); }
	int capacity() const { return capacity_; }
	void clear();

private:
	int capacity_ = 0;
	std::vector<int> free_;
	std::vector<char> used_; // guards double release, which would alias two chunks' geometry
};

} // namespace ve
