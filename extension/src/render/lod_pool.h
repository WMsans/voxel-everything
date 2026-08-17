#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include <vector>
#include "lod/lod_arena.h"
#include "lod/lod_contour.h"
#include "world/region.h"

namespace godot {

// Render-device side of the LoD page arena. A page is 512 packed 12-byte quads; a chunk's
// pages need not be contiguous. All uploads are buffer_update calls, so callers must record
// them before opening any compute/draw list (M2 Task 12's ordering rule).
class LodPool {
public:
	~LodPool();

	bool initialize(RenderingDevice *rd, int max_pages);
	void teardown();

	// All-or-nothing: either every page the quads need is allocated and uploaded, or nothing
	// happens and `pages_out` is left untouched.
	bool upload(int level, ve::IVec3 coord, const std::vector<ve::LodQuad> &quads,
			std::vector<int> *pages_out);
	void release(const std::vector<int> &pages);

	RID quad_buffer() const { return quads_; }
	RID index_buffer() const { return index_; }
	RID page_chunk_buffer() const { return page_chunk_; }
	RID chunk_buffer() const { return chunks_; }
	RID args_buffer() const { return args_; }

	int page_count() const { return arena_.capacity(); }
	int free_pages() const { return arena_.free_pages(); }

private:
	static constexpr int kChunkRecords = 8192;
	static constexpr uint32_t kNoChunk = 0xffffffffu;

	int allocate_chunk_slot();
	void release_chunk_slot(int slot);

	RenderingDevice *rd_ = nullptr;
	int max_pages_ = 0;
	ve::LodArena arena_{0};
	RID quads_;
	RID index_;
	RID page_chunk_;
	RID page_quads_;
	RID chunks_;
	RID args_;
	std::vector<uint32_t> page_chunk_cpu_; // page -> chunk-record slot; kNoChunk = free
	std::vector<uint32_t> page_quads_cpu_; // quads stored in that page (diagnostic/release)
	std::vector<int> free_chunk_slots_;
	std::vector<char> chunk_used_;
};

} // namespace godot
