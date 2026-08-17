#include "lod/lod_arena.h"
#include "lod/lod_contour.h"
#include <algorithm>

namespace ve {

int lod_pages_for_quads(int quads) {
	if (quads <= 0) return 0;
	const int pages = (quads + kLodQuadsPerPage - 1) / kLodQuadsPerPage;
	return std::min(pages, kLodMaxPagesPerChunk);
}

LodArena::LodArena(int page_count) : capacity_(std::max(0, page_count)) {
	clear();
}

void LodArena::clear() {
	free_.clear();
	free_.reserve(size_t(capacity_));
	// Descending, so pop_back hands out page 0 first: a fresh world's first chunks land at
	// the front of the buffer, which makes a hex dump of the arena readable.
	for (int i = capacity_ - 1; i >= 0; i--) free_.push_back(i);
	used_.assign(size_t(capacity_), 0);
}

bool LodArena::alloc(int pages, std::vector<int> *out) {
	if (!out) return false;
	out->clear();
	if (pages <= 0) return true;
	if (pages > free_pages()) return false;
	out->reserve(size_t(pages));
	for (int i = 0; i < pages; i++) {
		const int p = free_.back();
		free_.pop_back();
		used_[size_t(p)] = 1;
		out->push_back(p);
	}
	return true;
}

void LodArena::release(const std::vector<int> &pages) {
	for (int p : pages) {
		if (p < 0 || p >= capacity_) continue;
		if (!used_[size_t(p)]) continue; // already free: inert, never a second free-list entry
		used_[size_t(p)] = 0;
		free_.push_back(p);
	}
}

} // namespace ve
