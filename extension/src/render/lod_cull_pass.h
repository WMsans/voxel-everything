#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "render/async_readback.h"

namespace godot {

class HizPass;
class LodPool;

// Removes far-field pages from the indirect draw by zeroing their instanceCount. The CPU
// walk has already decided which pages are candidates, so this pass never adds pages; it
// only ever removes. Keeping draw_count an exact CPU integer matters because Godot's
// draw_list_draw_indirect takes the count as a parameter and exposes no count buffer.
//
// Trigger 1 (temporal second phase): the pass also reads back the remaining indirect args
// asynchronously and exposes the combined "last visible pages" set (first-pass pages plus
// remaining survivors) so RaymarchCompositor can draw that set first on the next frame.
class LodCullPass {
public:
	~LodCullPass();

	bool initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }

	// Records the pages drawn in this frame's temporal first pass. They are combined with
	// the stale remaining-pages readback when it arrives, forming last_visible_pages().
	void set_first_pass_pages(const std::vector<int> &pages) { first_pass_pages_ = pages; }
	const std::vector<int> &last_visible_pages() const { return last_visible_pages_; }
	// Replaces last_visible_pages() directly. Used when the remaining pass is empty and no
	// async args readback will arrive to refresh the visible set.
	void set_last_visible_pages(const std::vector<int> &pages);

	// Records the stats clear, one compute dispatch (one thread per candidate page), and
	// async stats + args readbacks. `page_count` is the number of remaining pages in the
	// currently uploaded args buffer; `total_page_count` and `first_pass_count` describe the
	// whole candidate set so culled_ratio() keeps counting pages drawn in the first pass.
	// The caller must have already uploaded the indirect args with
	// LodPool::upload_draw_args, and must end this compute list before opening the raster
	// draw list.
	bool run(RenderingDevice *rd, LodPool &pool, HizPass *hiz,
			const Projection &view_proj, int page_count, int total_page_count,
			int first_pass_count);

	RID stats_buffer() const { return stats_; }
	int last_drawn() const { return last_drawn_; }
	int last_total() const { return last_total_; }
	float culled_ratio() const {
		return last_total_ > 0 ? 1.0f - static_cast<float>(last_drawn_) / static_cast<float>(last_total_) : 0.0f;
	}
	// CPU command-record time only: std::chrono around run()'s command recording, not GPU
	// execution time. VoxelWorld::debug_perf_stats() reports this as lod_ms.
	float last_ms() const { return last_ms_; }

private:
	bool ensure_uniform_set(RenderingDevice *rd, LodPool &pool, HizPass *hiz);
	void consume_args_readback();

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_, stats_, uset_;
	RID uset_args_, uset_page_chunk_, uset_chunks_, uset_hiz_, uset_stats_;
	Ref<AsyncBufferRead> stats_readback_;
	Ref<AsyncBufferRead> args_readback_;
	std::vector<int> first_pass_pages_;
	std::vector<int> first_pass_pages_at_request_;
	std::vector<int> last_visible_pages_;
	int last_drawn_ = 0;
	int last_total_ = 0;
	int last_total_at_request_ = 0;
	int last_first_pass_count_at_request_ = 0;
	int last_remaining_count_at_request_ = 0;
	float last_ms_ = 0.0f;
};

} // namespace godot
