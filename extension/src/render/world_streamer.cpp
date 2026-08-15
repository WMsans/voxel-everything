#include "render/world_streamer.h"
#include "voxel_world.h" // godot::PendingEdit
#include <algorithm>

// An SDF edit that must allocate atlas slots is granted this many free slots before the
// streamer evicts a far region to make room (spec §8 "evicts or drops"). The largest net
// demand in the demo tests is a crater (~100 bricks); one eviction frees ~1k.
constexpr int kEditSlotHeadroom = 128;

// The share of the atlas streaming will not spend, so that edits always have slots to
// allocate from. Sized as a fraction because the atlas is a configuration knob; the floor
// keeps it meaningful on small test atlases and the ceiling keeps it from swallowing one.
//
// It exists because the atlas and the residency radius are sized independently, and at the
// shipping radius the atlas is the smaller of the two by more than 2x: the surface shell of
// a 96 m ball wants ~140k bricks against 65536 slots. Without a reserve, plain streaming
// spends the last slot, and from then on brick_mark.comp.glsl's allocate phase finds an
// empty free list and fail-softly DROPS every brick an edit activates. The terrain around
// each edit shatters into floating slabs, permanently.
static int stream_slot_reserve(int slot_count) {
	return std::min(slot_count / 4, std::max(1024, slot_count / 16));
}

// What one region's stream-in is assumed to cost the atlas, used to cap how many loads may
// be started before their true cost is known — the mark pass allocates on the GPU, so the
// bill only arrives next frame. A region holds bricks only where the surface crosses it,
// ~1.5-2.5k of its 32768 for the demo's hills; the estimate is deliberately on the high
// side, since over-estimating merely streams a little more slowly while under-estimating
// spends the reserve the edits live on.
constexpr int kSlotsPerRegionEstimate = 3072;

// How many regions the repair sweep re-marks per frame. A forced re-mark re-enqueues every
// active brick in the region, so this is bounded by the job list (max_brick_jobs) as much as
// by the frame budget: two regions is ~4k jobs against a 16k list.
constexpr int kRepairRegionsPerFrame = 2;

// How many regions to give back per frame while the atlas is reporting dropped bricks. Two
// converges in well under a second at 60 fps and keeps the give-back gentle enough that a
// single starved frame does not visibly shorten the horizon.
constexpr int kShrinkRegionsPerFrame = 2;

using namespace godot;

void WorldStreamer::initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
		std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
		RegionPass *region_pass, BrickGenPass *brick_gen) {
	residency_ = residency;
	edit_log_ = edit_log;
	edit_mutex_ = edit_mutex;
	pending_ = pending;
	atlas_ = atlas;
	region_pass_ = region_pass;
	brick_gen_ = brick_gen;
}

int WorldStreamer::run_frame(RenderingDevice *rd, float cx, float cy, float cz) {
	frame_edits_ = 0;
	// The edit preview is one frame of feedback: clear it every frame, and only re-arm it
	// below for edits that actually produced an on-screen job. Otherwise a single edit
	// would tint the scene forever and an all-off-screen edit would keep the PREVIOUS
	// edit's preview alive.
	last_edit_radius_ = 0.0f;
	last_edit_center_[0] = 0.0f;
	last_edit_center_[1] = 0.0f;
	last_edit_center_[2] = 0.0f;
	last_edit_type_ = 0;
	last_edit_material_ = 0;
	if (!rd || !atlas_ || !residency_ || !edit_log_ || !region_pass_ || !brick_gen_) return 0;

	// Both counters are read once, here, and reused for the rest of the frame: nothing this
	// function records executes before the caller submits, so a second read would return the
	// same bytes at the price of another readback stall.
	const uint32_t overflow = atlas_->read_overflow(rd);
	const int free_slots = atlas_->read_free_count(rd);

	// Overflow recovery, fast path (brick_mark.comp.glsl contract): the job list overflowed
	// (bit 1, value 2), leaving bricks resident-but-ungenerated in stale atlas bytes. Re-mark
	// last frame's marked regions with force_regen below so the dropped bricks are
	// re-enqueued — one frame of stale bytes is the documented cost.
	const bool overflow_regen = (overflow & 2u) != 0;

	// Overflow recovery, slow path. Either bit means bricks are missing or stale somewhere,
	// and the fast path alone cannot be trusted to find them: the counter is read back on
	// the render thread, where nothing stalls for a submit, so by the time a bit surfaces the
	// regions it refers to may be several frames back and no longer in pending_regen_. And
	// unlike a stale brick, a DROPPED one (bit 0, the free list ran empty) is never revisited
	// by anything at all — no load or edit range covers it again, so the player is left
	// looking at sky through the ground for as long as the region stays resident.
	//
	// So on either bit, queue every CURRENTLY resident region for a forced re-mark, replacing
	// any sweep still in progress. Replacing rather than skipping is what makes this
	// converge: a sweep started mid-flight only knows the regions resident when it began, so
	// keeping it would leave everything that arrived since unhealed. The sweep is drained a
	// couple of regions per frame, and once the shortage passes, the last one queued runs to
	// completion over the resident set.
	if ((overflow & 3u) != 0) residency_->resident_regions(&repair_queue_);
	// The word is sticky (GpuAtlas::reset_frame_counters); clear it now that both arms have
	// been read, so the next frame reports what the next frame's marks actually hit.
	if (overflow != 0u) atlas_->clear_overflow(rd);

	// Drain the edit queue. The lock is held for a swap, never across GPU work.
	std::vector<PendingEdit> edits;
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		edits.swap(*pending_);
	}

	// Pace this frame's stream-in against what the atlas can pay for. Two brakes, and both
	// are needed: the budget bounds how many loads may be in flight before their true cost
	// lands, so the free list cannot be spent in a single frame; the reserve decides when
	// the resident set has reached the atlas' capacity, after which a further region may
	// only be taken by DISPLACING the furthest resident — an exchange that returns that
	// region's bricks in the same compute list, before the load claims new ones. The set
	// stays nearest-first, so what stops arriving is the far horizon, never the ground the
	// player is editing. (RegionResidency::update exempts an empty world from scarcity, so a
	// starved first frame cannot deadlock the bootstrap.)
	const int reserve = stream_slot_reserve(atlas_->atlas_slot_count());
	int inflight = 0;
	for (int i = 0; i < kInflightFrames; i++) inflight += inflight_loads_[i];
	const int budget_slots = free_slots - inflight * kSlotsPerRegionEstimate;
	// Both decisions run off the debited figure, not the raw readback. Judging scarcity on
	// the raw count would let the streamer keep taking fresh ground on the strength of slots
	// that loads already in flight have spoken for — which is exactly how the free list gets
	// to zero — and would leave it stalling outright rather than switching to the
	// slot-neutral displacement that always makes progress.
	const bool bricks_scarce = budget_slots <= reserve;
	const int load_budget = std::max(1, budget_slots / kSlotsPerRegionEstimate);
	ve::ResidencyPlan plan = residency_->update(cx, cy, cz, bricks_scarce, load_budget);
	inflight_head_ = (inflight_head_ + 1) % kInflightFrames;
	inflight_loads_[inflight_head_] = static_cast<int>(plan.loads.size());
	frame_edits_ = static_cast<int>(edits.size());

	// Shrink. The reserve only stops the resident set GROWING; a set that is already over
	// the atlas' capacity — which is what a run of starved frames leaves behind, since the
	// free count understates the demand of regions whose bricks were dropped — would
	// otherwise sit there forever, every frame dropping the bricks it cannot place and the
	// repair sweep fighting for slots it will never get. A reported drop is the signal that
	// the set is too big for the atlas, so give the furthest regions back until the drops
	// stop. Regions loaded this frame are excluded: evicting one would undo the load.
	if ((overflow & 1u) != 0) {
		std::vector<ve::IVec3> keep;
		for (const auto &l : plan.loads) keep.push_back(l.region);
		for (int i = 0; i < kShrinkRegionsPerFrame; i++)
			if (!residency_->evict_furthest(cx, cy, cz, &plan, keep)) break;
	}

	// Edit headroom (spec §8 "evicts or drops"). The atlas is sized "tight" by design, and
	// at the demo camera the resident set fills it, so an SDF-changing edit that net-ADDS
	// surface bricks (a crater has more wall area than the disk it removes) cannot allocate
	// and the drop arm alone would leave the edit invisible in the raymarcher. Evict the
	// furthest resident region this frame's edits do NOT touch and that was NOT loaded this
	// frame so their allocations succeed; the evicted region re-streams next frame with the
	// new ops. Paint edits change no SDF, so they can never need slots and never trigger
	// this. One eviction frees ~1k slots — far more than any single edit in the demo needs.
	if (!edits.empty()) {
		std::vector<ve::IVec3> edit_resident;
		std::vector<ve::IVec3> exclude;
		bool sdf_edit = false;
		for (const auto &pe : edits) {
			if (pe.op.type != ve::kOpSpherePaint) sdf_edit = true;
			for (const ve::IVec3 &r : pe.result.touched)
				if (residency_->slot_of(r) >= 0) edit_resident.push_back(r);
		}
		// The evicted region must not be one this frame just loaded. update() displaced the
		// furthest residents to make room for candidates, so a freshly loaded candidate is by
		// construction the furthest resident and evict_furthest would pick it — but its own
		// load mark re-allocates the very bricks the eviction freed, BEFORE the edit's
		// force-marks. The arm would free zero net headroom and the edit would still hit the
		// empty free list and silently drop (the symptom Errata 11 fixed). Only a steady
		// resident the edit does not touch is evictable: its slot's bricks stay in the free
		// list until the edit's marks take them.
		exclude = edit_resident;
		for (const auto &l : plan.loads)
			if (std::find(exclude.begin(), exclude.end(), l.region) == exclude.end())
				exclude.push_back(l.region);
		if (sdf_edit && !edit_resident.empty() && free_slots < kEditSlotHeadroom)
			residency_->evict_furthest(cx, cy, cz, &plan, exclude);
	}

	// --- buffer_update phase (must precede compute_list_begin: Godot errors otherwise) ---
	// edit_mutex_ guards edit_log_ (voxel_world.h): the tool thread appends ops while this
	// render thread reads, so each ops() read + its pool upload must be atomic. buffer_update
	// is CPU-side recording, not GPU work — microseconds under the lock is fine. The op
	// count is captured here too so the mark loop below needs no second (locked) read.
	struct LoadJob { ve::IVec3 region; int slot; int op_count; };
	std::vector<LoadJob> load_jobs;
	for (const auto &l : plan.loads) {
		int op_count = 0;
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			const std::vector<ve::EditOp> &ops = edit_log_->ops(l.region);
			op_count = static_cast<int>(ops.size());
			// Unconditional, even when empty: the slot may be a recycled eviction still
			// holding the previous tenant's op count, and op_count is the only field the
			// mark pass reads.
			atlas_->upload_region_ops(rd, l.slot, ops.data(), op_count);
		}
		atlas_->set_region_map_entry(rd, l.map_index, l.slot);
		load_jobs.push_back({l.region, l.slot, op_count});
	}
	for (const auto &e : plan.evicts)
		atlas_->set_region_map_entry(rd, e.map_index, -1);

	// Edits re-mark only the op's brick AABB clamped to each touched region. An op that
	// touches a region's APRON plane is in that region's list by construction (Task 2's
	// one-voxel pad), so the GPU probe — which always uses the brick's OWN region list —
	// sees every op that can change any of its 27 probe points.
	struct EditJob { ve::IVec3 region; int slot; ve::IVec3 lo, hi; int op_count; };
	std::vector<EditJob> edit_jobs;
	for (const auto &pe : edits) {
		ve::IVec3 blo{}, bhi{};
		ve::op_brick_range(pe.op, &blo, &bhi);
		for (const ve::IVec3 &region : pe.result.touched) {
			const int slot = residency_->slot_of(region);
			if (slot < 0) continue; // off-screen edit: bricks regenerate on stream-in
			// Same race as the load loop: the tool thread may be rebalancing the region
			// map or reallocating this region's list while we memcpy it into the pool.
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
			atlas_->upload_region_ops(rd, slot, ops.data(), static_cast<int>(ops.size()));
			const ve::IVec3 r0{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			const EditJob job{region, slot,
					{std::max(blo.x, r0.x), std::max(blo.y, r0.y), std::max(blo.z, r0.z)},
					{std::min(bhi.x, r0.x + 31), std::min(bhi.y, r0.y + 31),
							std::min(bhi.z, r0.z + 31)},
					static_cast<int>(ops.size())};
			if (job.lo.x <= job.hi.x && job.lo.y <= job.hi.y && job.lo.z <= job.hi.z) {
				edit_jobs.push_back(job);
				// Re-armed only here: the edit is on screen (it produced a job) and the
				// preview was cleared at the top of this frame, so it shows for one frame.
				last_edit_center_[0] = pe.op.pos[0];
				last_edit_center_[1] = pe.op.pos[1];
				last_edit_center_[2] = pe.op.pos[2];
				last_edit_radius_ = pe.op.radius;
				last_edit_type_ = static_cast<int>(pe.op.type);
				last_edit_material_ = static_cast<int>(pe.op.material);
			}
		}
	}

	atlas_->reset_frame_counters(rd);

	// --- compute phase: ONE list, no submit (the caller's frame submits) ---
	const int64_t list = rd->compute_list_begin();
	bool any = false;
	for (const auto &e : plan.evicts) {
		region_pass_->release_region(rd, list, e.slot);
		any = true;
	}
	if (any) rd->compute_list_add_barrier(list); // frees must not collide with pops below
	for (const auto &lj : load_jobs) {
		// op_count was captured under the lock in the buffer_update phase; no re-read.
		const ve::IVec3 lo{lj.region.x * ve::kRegionBricks, lj.region.y * ve::kRegionBricks,
				lj.region.z * ve::kRegionBricks};
		const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
		region_pass_->mark(rd, list, lj.region, lj.slot, lo, hi, lj.op_count, false);
		any = true;
	}
	for (const auto &j : edit_jobs) {
		// mark(force=true) records release, barrier, allocate; the extra barrier BEFORE
		// each job keeps one job's release phase from racing the previous job's allocate
		// phase when two edits share bricks (the race the phase split exists to avoid).
		rd->compute_list_add_barrier(list);
		region_pass_->mark(rd, list, j.region, j.slot, j.lo, j.hi, j.op_count, true);
		any = true;
	}
	// Overflow recovery: re-mark the regions marked LAST frame with force_regen so the
	// bricks that overflowed the job list are re-enqueued. The op count is read under the
	// lock (microseconds) but the list recording happens outside it — never GPU work under
	// the mutex. Regions evicted since are skipped: they regenerate on stream-in.
	if (overflow_regen) {
		for (const ve::IVec3 &region : pending_regen_) {
			const int slot = residency_->slot_of(region);
			if (slot < 0) continue;
			int op_count = 0;
			{
				std::lock_guard<std::mutex> lock(*edit_mutex_);
				op_count = static_cast<int>(edit_log_->ops(region).size());
			}
			const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
			rd->compute_list_add_barrier(list);
			region_pass_->mark(rd, list, region, slot, lo, hi, op_count, true);
			any = true;
		}
	}
	// Drain the repair queue: the regions that lost bricks to an empty free list, forced
	// back through the mark pass now that there are slots again. Paced so a whole resident
	// set is healed over a second or so rather than in one frame. The gate is the edit
	// headroom, NOT the stream reserve: once the resident set has reached the atlas'
	// capacity the free count settles just below the reserve by construction, so gating the
	// repair on being above it would mean the holes never healed at all.
	int repairs = 0;
	while (!repair_queue_.empty() && repairs < kRepairRegionsPerFrame &&
			free_slots > kEditSlotHeadroom) {
		const ve::IVec3 region = repair_queue_.back();
		repair_queue_.pop_back();
		const int slot = residency_->slot_of(region);
		if (slot < 0) continue; // evicted since: it regenerates on stream-in
		int op_count = 0;
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			op_count = static_cast<int>(edit_log_->ops(region).size());
		}
		const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
				region.z * ve::kRegionBricks};
		const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
		rd->compute_list_add_barrier(list);
		region_pass_->mark(rd, list, region, slot, lo, hi, op_count, true);
		any = true;
		repairs++;
	}

	if (any) {
		rd->compute_list_add_barrier(list);
		region_pass_->write_dispatch_args(rd, list);
		// compute_list_add_barrier() restarts the list and REPLAYS the last set push
		// constant against the last bound pipeline, so write_dispatch_args pushes a
		// matching 16-byte constant (dispatch_args.comp.glsl declares one) — otherwise
		// this replay would push the mark pass's 64 bytes into a 0-byte pipeline and
		// Godot errors. The barrier itself is the submission boundary that makes the
		// args write visible to the indirect dispatch below (removing it broke the
		// pixel/magenta suites: stale job counts left bricks ungenerated).
		rd->compute_list_add_barrier(list);
		brick_gen_->dispatch(rd, list, *atlas_);
	}
	rd->compute_list_end();

	// Track the regions marked THIS frame (loads + edit jobs, deduped) so a job-list
	// overflow can be recovered by force-regen next frame. Replaces last frame's list.
	pending_regen_.clear();
	for (const auto &lj : load_jobs) pending_regen_.push_back(lj.region);
	for (const auto &j : edit_jobs) {
		bool dup = false;
		for (const auto &r : pending_regen_)
			if (r == j.region) { dup = true; break; }
		if (!dup) pending_regen_.push_back(j.region);
	}
	return static_cast<int>(plan.loads.size() + plan.evicts.size() + edit_jobs.size());
}
