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
#include <utility>

namespace godot {

struct PendingEdit;      // defined in core/world_store.h; here only the pointer type is needed
struct OccupancyBlock;   // defined in core/world_store.h; here only the pointer type is needed
class WorldStore;
class MeshService;
class FieldContextSet;

// Drives one frame of world maintenance on the render thread: residency loads/evictions,
// edit fan-out, one compute list holding every mark + the indirect generation dispatch.
// Owns nothing; every pointer is borrowed from VoxelWorld.
class WorldStreamer {
public:
	void initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
			std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
			RegionPass *region_pass, BrickGenPass *brick_gen, WorldStore *store,
			const ve::OverrideStore *overrides,
			const std::map<std::tuple<int, int, int>, int> *override_tables,
			const FieldContextSet *field_context = nullptr);
	void set_mesh_service(MeshService *mesh) { mesh_ = mesh; }
	void set_region_gate(ve::RegionGate g) { region_gate_ = std::move(g); }
	// A consolidation changes the base bytes without leaving an edit for the normal edit
	// fan-out. Queue a full-region force mark so the render atlas cannot retain pre-bake data.
	void queue_region_regeneration(ve::IVec3 region);
	// Caller holds edit_mutex_; used by the atomic consolidation commit.
	void queue_region_regeneration_locked(ve::IVec3 region);
	// Regions a consolidation forced, for debug_edit_fanout. Caller holds edit_mutex_.
	const std::vector<ve::IVec3> &forced_regen() const { return forced_regen_; }

	// Eight reads in flight. buffer_get_data_async costs the frame that asks nothing, but
	// the bytes turn up a few frames later, so the only way to shorten the delay between an
	// edit and its occupancy is to have several outstanding at once. Eight covers the region
	// fan-out of the demo's largest blast with one request each.
	static constexpr int kOccupancyReads = 8;

	// The bricks a frame may generate streaming regions in (see run_frame). Measured on an
	// Iris Xe, a thousand bricks cost 5 ms of GPU in Mesas, 9 in Default and 16 in Fjords;
	// generating four whole regions in the frame they loaded was the 40-130 ms hitch a moving
	// camera paid every few frames. At 512 the moving camera's stream-in keeps up in all three.
	static constexpr int kStreamJobBudget = 512;

	// Returns the number of actions taken (loads + evicts + slab allocations + regions still
	// staged + edit-mark jobs), so a caller waiting for quiet waits out a staged region too.
	// Records ONE compute list; the caller submits. buffer_update calls happen before the
	// list opens. `job_budget` < 0 marks every loaded region whole in its load frame, as the
	// hook-driven test frames always have.
	int run_frame(RenderingDevice *rd, float cx, float cy, float cz,
			int job_budget = kStreamJobBudget);
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
	// The orchestrator's set 1, borrowed; bound at every mark/generation dispatch.
	const FieldContextSet *field_context_ = nullptr;
	MeshService *mesh_ = nullptr;
	ve::RegionGate region_gate_;
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
	// Owns occupancy handoff (enqueue_occupancy_block) and the edit_seq atomic the marks are
	// stamped with. Borrowed from VoxelWorld.
	WorldStore *store_ = nullptr;

	// A region loaded but not yet published: its slot is assigned, its tables set and its
	// surface bricks probed and tagged, and its slabs are allocated over the following frames
	// as the job budget allows. It enters the region map in the frame its last slab is
	// allocated, so the renderer never sees part of one.
	struct StagedRegion {
		ve::IVec3 region{};
		int slot = -1;
		int map_index = -1;
		// The frame its probe ran: its slab counts are known once a readback requested after
		// that frame has landed.
		int64_t probe_frame = 0;
		int next_slab = 0;
		// A repair or overflow re-mark arrived while staged: re-mark from slab 0 with
		// force_regen, which probes again and re-generates the slabs already allocated.
		bool force = false;
	};
	std::vector<StagedRegion> staged_;
	// run_frame calls; the clock the slab-count readback is dated by.
	int64_t frame_ = 0;
	// GpuAtlas::slab_counts as last read back (max_region_slots x ve::kStreamSlabs), and the
	// frame the request that delivered it was issued in. It covers every probe of an earlier
	// frame.
	std::vector<int> slab_counts_;
	Ref<AsyncBufferRead> slab_counts_read_;
	int64_t slab_counts_requested_frame_ = -1;
	int64_t slab_counts_valid_frame_ = -1;
	// Restart a staged region's slabs with force_regen. False if the region is not staged.
	bool restage_forced(ve::IVec3 region);

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
