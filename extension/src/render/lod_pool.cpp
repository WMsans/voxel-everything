#include "render/lod_pool.h"
#include "lod/lod_grid.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

LodPool::~LodPool() {
	teardown();
}

bool LodPool::initialize(RenderingDevice *rd, int max_pages) {
	teardown();
	if (!rd || max_pages <= 0) return false;
	rd_ = rd;
	max_pages_ = max_pages;
	arena_ = ve::LodArena(max_pages);

	// The arena is large at shipping defaults (32768 pages * 6 KB = 192 MB). Pass no initial
	// data so we do not also stage a CPU-side copy; only pages that are actually uploaded are
	// ever written.
	const int64_t quads_bytes = static_cast<int64_t>(max_pages) * ve::kLodQuadsPerPage *
			ve::kLodQuadBytes;
	quads_ = rd_->storage_buffer_create(static_cast<uint32_t>(quads_bytes));

	PackedByteArray index_data;
	index_data.resize(ve::kLodQuadsPerPage * 6 * 2);
	uint16_t *idx = reinterpret_cast<uint16_t *>(index_data.ptrw());
	for (int q = 0; q < ve::kLodQuadsPerPage; q++) {
		const uint16_t base = static_cast<uint16_t>(q * 4);
		idx[q * 6 + 0] = base;
		idx[q * 6 + 1] = base + 1;
		idx[q * 6 + 2] = base + 2;
		idx[q * 6 + 3] = base;
		idx[q * 6 + 4] = base + 2;
		idx[q * 6 + 5] = base + 3;
	}
	index_ = rd_->index_buffer_create(ve::kLodQuadsPerPage * 6,
			RenderingDevice::INDEX_BUFFER_FORMAT_UINT16, index_data);

	const int64_t page_entries = static_cast<int64_t>(max_pages) * 4;
	PackedByteArray zero;
	zero.resize(page_entries);
	page_chunk_ = rd_->storage_buffer_create(static_cast<uint32_t>(page_entries), zero);
	page_quads_ = rd_->storage_buffer_create(static_cast<uint32_t>(page_entries), zero);

	PackedByteArray chunk_zero;
	chunk_zero.resize(static_cast<int64_t>(kChunkRecords) * 32);
	chunks_ = rd_->storage_buffer_create(static_cast<uint32_t>(chunk_zero.size()), chunk_zero);

	PackedByteArray args_zero;
	args_zero.resize(static_cast<int64_t>(max_pages) * 20);
	args_ = rd_->storage_buffer_create(static_cast<uint32_t>(args_zero.size()), args_zero,
			RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);

	if (!quads_.is_valid() || !index_.is_valid() || !page_chunk_.is_valid() ||
			!page_quads_.is_valid() || !chunks_.is_valid() || !args_.is_valid()) {
		UtilityFunctions::printerr("LodPool: buffer creation failed");
		teardown();
		return false;
	}

	page_chunk_cpu_.assign(static_cast<size_t>(max_pages), kNoChunk);
	page_quads_cpu_.assign(static_cast<size_t>(max_pages), 0);
	free_chunk_slots_.clear();
	free_chunk_slots_.reserve(kChunkRecords);
	for (int i = kChunkRecords - 1; i >= 0; i--) free_chunk_slots_.push_back(i);
	chunk_used_.assign(kChunkRecords, 0);
	return true;
}

void LodPool::teardown() {
	if (rd_) {
		if (quads_.is_valid()) rd_->free_rid(quads_);
		if (index_.is_valid()) rd_->free_rid(index_);
		if (page_chunk_.is_valid()) rd_->free_rid(page_chunk_);
		if (page_quads_.is_valid()) rd_->free_rid(page_quads_);
		if (chunks_.is_valid()) rd_->free_rid(chunks_);
		if (args_.is_valid()) rd_->free_rid(args_);
	}
	quads_ = RID();
	index_ = RID();
	page_chunk_ = RID();
	page_quads_ = RID();
	chunks_ = RID();
	args_ = RID();
	rd_ = nullptr;
	max_pages_ = 0;
	arena_ = ve::LodArena(0);
	page_chunk_cpu_.clear();
	page_quads_cpu_.clear();
	free_chunk_slots_.clear();
	chunk_used_.clear();
}

int LodPool::allocate_chunk_slot() {
	if (free_chunk_slots_.empty()) return -1;
	const int slot = free_chunk_slots_.back();
	free_chunk_slots_.pop_back();
	chunk_used_[static_cast<size_t>(slot)] = 1;
	return slot;
}

void LodPool::release_chunk_slot(int slot) {
	if (slot < 0 || slot >= static_cast<int>(chunk_used_.size()) ||
			!chunk_used_[static_cast<size_t>(slot)])
		return;
	chunk_used_[static_cast<size_t>(slot)] = 0;
	free_chunk_slots_.push_back(slot);
}

bool LodPool::upload(int level, ve::IVec3 coord, const std::vector<ve::LodQuad> &quads,
		std::vector<int> *pages_out) {
	if (!rd_ || !quads_.is_valid() || quads.empty() || !pages_out) return false;
	const int pages_needed = ve::lod_pages_for_quads(static_cast<int>(quads.size()));
	if (pages_needed <= 0 || pages_needed > arena_.free_pages()) return false;

	const int chunk_slot = allocate_chunk_slot();
	if (chunk_slot < 0) return false;

	std::vector<int> pages;
	if (!arena_.alloc(pages_needed, &pages)) {
		release_chunk_slot(chunk_slot);
		return false;
	}

	float origin[3];
	ve::lod_chunk_origin(level, coord, origin);
	const float cell = ve::lod_cell_size(level);

	// Chunk record: two vec4 = (origin.xyz, cell), (uint level, uint flags, uint pad, uint pad).
	PackedByteArray chunk_bytes;
	chunk_bytes.resize(32);
	float *rec = reinterpret_cast<float *>(chunk_bytes.ptrw());
	rec[0] = origin[0];
	rec[1] = origin[1];
	rec[2] = origin[2];
	rec[3] = cell;
	// The second vec4 is integer data; write it through a uint32 view so the shader's
	// `uint level` reads the actual level, not a float bit pattern.
	uint32_t *meta = reinterpret_cast<uint32_t *>(chunk_bytes.ptrw()) + 4;
	meta[0] = static_cast<uint32_t>(level);
	meta[1] = 0u; // flags
	meta[2] = 0u; // pad
	meta[3] = 0u; // pad
	rd_->buffer_update(chunks_, static_cast<uint32_t>(chunk_slot) * 32, 32, chunk_bytes);

	PackedByteArray word;
	word.resize(4);
	for (int i = 0; i < pages_needed; i++) {
		const int page = pages[static_cast<size_t>(i)];
		const int first = i * ve::kLodQuadsPerPage;
		const int count = std::min(ve::kLodQuadsPerPage,
				static_cast<int>(quads.size()) - first);

		PackedByteArray quad_bytes;
		quad_bytes.resize(static_cast<int64_t>(count) * ve::kLodQuadBytes);
		if (count > 0)
			std::memcpy(quad_bytes.ptrw(), quads.data() + first,
					static_cast<size_t>(count) * ve::kLodQuadBytes);
		rd_->buffer_update(quads_, static_cast<uint32_t>(page) * ve::kLodQuadsPerPage *
						ve::kLodQuadBytes,
				static_cast<uint32_t>(quad_bytes.size()), quad_bytes);

		const uint32_t ci = static_cast<uint32_t>(chunk_slot);
		std::memcpy(word.ptrw(), &ci, 4);
		rd_->buffer_update(page_chunk_, static_cast<uint32_t>(page) * 4, 4, word);
		page_chunk_cpu_[static_cast<size_t>(page)] = ci;

		const uint32_t qc = static_cast<uint32_t>(count);
		std::memcpy(word.ptrw(), &qc, 4);
		rd_->buffer_update(page_quads_, static_cast<uint32_t>(page) * 4, 4, word);
		page_quads_cpu_[static_cast<size_t>(page)] = qc;
	}

	pages_out->assign(pages.begin(), pages.end());
	return true;
}

void LodPool::release(const std::vector<int> &pages) {
	if (!rd_ || pages.empty()) return;
	arena_.release(pages);
	// Free each chunk record once. Pages passed together normally belong to one chunk; the
	// first page's CPU slot is the chunk record, and releasing it is enough.
	int released_slot = -1;
	for (int p : pages) {
		if (p < 0 || p >= static_cast<int>(page_chunk_cpu_.size())) continue;
		const uint32_t ci = page_chunk_cpu_[static_cast<size_t>(p)];
		page_chunk_cpu_[static_cast<size_t>(p)] = kNoChunk;
		page_quads_cpu_[static_cast<size_t>(p)] = 0;
		if (ci != kNoChunk) {
			const int slot = static_cast<int>(ci);
			if (slot != released_slot) {
				release_chunk_slot(slot);
				released_slot = slot;
			}
		}
	}
}
