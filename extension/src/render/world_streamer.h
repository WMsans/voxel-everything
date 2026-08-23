#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <atomic>
#include <mutex>
#include <vector>
#include "generator/generator.h"
#include "render/async_readback.h"
#include "render/gpu_atlas.h"
#include "render/brick_gen_pass.h"
#include "render/region_pass.h"
#include "world/edit_log.h"
#include "world/override_store.h"
#include "world/residency.h"
#include <map>
#include <tuple>

namespace godot {

struct PendingEdit;      // defined in core/world_store.h; here only the pointer type is needed
struct OccupancyBlock;   // defined in voxel_world.h; here only the pointer type is needed
class MeshService;

// Drives one frame of world maintenance on the render thread: residency loads/evictions,
// edit fan-out, one compute list holding every mark + the indirect generation dispatch.
// Owns nothing; every pointer is borrowed from VoxelWorld.
class WorldStreamer {
public:
	void initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
			std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
			RegionPass *region_pass, BrickGenPass *brick_gen, std::mutex *occ_mutex,
			std::vector<OccupancyBlock> *occ_inbox, std::atomic<int64_t> *edit_seq,
			const ve::OverrideStore *overrides,
			const std::map<std::tuple<int, int, int>, int> *override_tables);
	void set_mesh_service(MeshService *mesh) { mesh_ = mesh; }
	// A consolidation changes the base bytes without leaving an edit for the normal edit
	// fan-out. Queue a full-region force mark so the render atlas cannot retain pre-bake data.
	void queue_region_regeneration(ve::IVec3 region);
	// Caller holds edit_mutex_; used by the atomic consolidation commit.
	void queue_region_regeneration_locked(ve::IVec3 region);

	// Eight reads in flight. buffer_get_data_async costs the frame that asks nothing, but
	// the bytes turn up a few frames later, so the only way to shorten the delay between an
	// edit and its occupancy is to have several outstanding at once. Eight covers the region
	// fan-out of the demo's largest blast with one request each.
	static constexpr int kOccupancyReads = 8;

	// Returns the number of actions taken (loads + evicts + edit-mark jobs). Records ONE
	// compute list; the caller submits. buffer_update calls happen before the list opens.
	int run_frame(RenderingDevice *rd, float cx, float cy, float cz);
	// RenderingDevice has no async-readback cancellation. Called on the owning render thread
	// before atlas buffers are freed so every pending Callable target is completed first.
	void drain_readbacks(RenderingDevice *rd);
	// Debug/test hook: harvest occupancy readbacks and queue the next reads without running a
	// streaming mark or opening a compute list.
	void harvest_occupancy(RenderingDevice *rd);

	int last_frame_edits() const { return frame_edits_; }
	// --- profiling (diagnostic only; see VoxelWorld::debug_perf_stats) ---
	float last_readback_ms() const { return last_readback_ms_; }
	float last_total_ms() const { return last_total_ms_; }
	// Sticky OR of every overflow word this streamer has read. The frame word is cleared as
	// soon as it is acted on, so this is the only place a player-facing HUD can learn that
	// the atlas ever came up short — VoxelWorld's own counter only ticks on the debug
	// stream path, which the running demo never takes.
	uint32_t overflow_seen() const { return overflow_seen_; }
	const float *last_edit_center() const { return last_edit_center_; }
	float last_edit_radius() const { return last_edit_radius_; }
	int last_edit_type() const { return last_edit_type_; }
	int last_edit_material() const { return last_edit_material_; }

private:
	ve::RegionResidency *residency_ = nullptr;
	ve::EditLog *edit_log_ = nullptr;
	std::mutex *edit_mutex_ = nullptr;
	std::vector<PendingEdit> *pending_ = nullptr;
	GpuAtlas *atlas_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *brick_gen_ = nullptr;
	MeshService *mesh_ = nullptr;
	const ve::OverrideStore *overrides_ = nullptr;
	const std::map<std::tuple<int, int, int>, int> *override_tables_ = nullptr;
	int frame_edits_ = 0;
	uint32_t overflow_seen_ = 0;
	// Regions marked last frame (loads + on-screen edit jobs). If the job list overflowed
	// (brick_mark.comp.glsl sets frame.overflow bit 1), the next run_frame re-marks these
	// with force_regen so the dropped bricks are re-enqueued (see run_frame).
	std::vector<ve::IVec3> pending_regen_;
	std::vector<ve::IVec3> forced_regen_;
	// Atlas slots held by each region slot, read back from the mark pass' tally once a frame
	// (max_region_slots ints). It is what makes an eviction's worth knowable: the streamer
	// funds every stream-in out of releases that actually return bricks, instead of assuming
	// a flat cost and hoping the furthest resident was holding some.
	std::vector<int> region_slot_costs_;
	// Regions still owed a forced re-mark after the free list ran dry. A dropped brick is
	// invisible and nothing else ever comes back for it, so the world would keep the hole
	// for as long as the region stays resident; this queue is what heals it once slots are
	// available again. Drained a couple of regions per frame — a repair, not a stampede.
	std::vector<ve::IVec3> repair_queue_;
	struct OccupancyRead {
		Ref<AsyncBufferRead> read;
		ve::IVec3 region{};
		int64_t seq = 0;
		bool active = false;
	};
	// Regions marked LAST frame, waiting for a free ring slot. Requests are issued at the
	// top of run_frame, before any compute list opens: buffer_get_data_async is a
	// device-level command under the same ordering rule as buffer_update (M2 Task 12), so a
	// request issued now returns the state as of the previous frame's mark -- which is
	// exactly what these entries describe.
	std::vector<OccupancyBlock> occ_pending_;  // region + seq only; bytes filled on arrival
	OccupancyRead occ_reads_[kOccupancyReads];
	std::mutex *occ_mutex_ = nullptr;
	std::vector<OccupancyBlock> *occ_inbox_ = nullptr;
	std::atomic<int64_t> *edit_seq_ = nullptr;

	void note_marked(ve::IVec3 region, int64_t seq);
	void pump_occupancy(RenderingDevice *rd);

	float last_edit_center_[3] = {0.0f, 0.0f, 0.0f};
	float last_edit_radius_ = 0.0f;
	int last_edit_type_ = 0;
	int last_edit_material_ = 0;
	float last_readback_ms_ = 0.0f;
	float last_total_ms_ = 0.0f;

	// --- asynchronous counter readback (see AsyncBufferRead) ---
	Ref<AsyncBufferRead> overflow_read_;
	Ref<AsyncBufferRead> free_read_;
	Ref<AsyncBufferRead> costs_read_;
	// The free count a request returned, and the running total of slots this streamer has
	// COMMITTED to loads. Because the returned count is a few frames stale, the slots spent
	// since that request went out are not in it; subtracting them is what keeps the estimate
	// pessimistic rather than optimistic. Optimistic is the direction that empties the free
	// list and makes brick_mark drop bricks out of freshly streamed ground.
	int cached_free_slots_ = 0;
	int64_t committed_ = 0;            // monotonic; only differences are ever used
	int64_t committed_at_request_ = 0; // committed_ when the outstanding request went out
	int64_t committed_base_ = 0;       // committed_ as of the reading now cached
};

} // namespace godot
