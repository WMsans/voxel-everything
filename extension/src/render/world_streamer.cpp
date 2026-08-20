#include "render/world_streamer.h"
#include "voxel_world.h" // godot::PendingEdit
#include "render/mesh_service.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace {
using Clock = std::chrono::steady_clock;
float ms_since(Clock::time_point t0) {
	return std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
}
} // namespace

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
//
// The reserve is also the margin that covers one frame of loads whose marks the free count
// has not been told about yet, which is why it is never smaller than a frame's worth.
static int stream_slot_reserve(int slot_count, int frame_load_cost) {
	const int base = std::min(slot_count / 4, std::max(1024, slot_count / 16));
	return std::min(slot_count / 4, std::max(base, frame_load_cost));
}

// The fallback cost of one region's stream-in, used only until the mark pass has reported
// what real regions cost (first frames) or on a degenerate readback. A region holds bricks
// only where the surface crosses it, ~1.5-2.5k of its 32768 for the demo's hills.
constexpr int kSlotsPerRegionFallback = 3072;
// The floor under the measured estimate. Without it a world whose first resident regions are
// all air would price a load at nothing and let the whole atlas go in one frame.
constexpr int kSlotsPerRegionFloor = 512;

// How many regions the repair sweep re-marks per frame. A forced re-mark re-enqueues every
// active brick in the region, so this is bounded by the job list (max_brick_jobs) as much as
// by the frame budget: two regions is ~4k jobs against a 16k list.
constexpr int kRepairRegionsPerFrame = 2;

// How many regions to give back per frame while the atlas is reporting dropped bricks. Two
// converges in well under a second at 60 fps and keeps the give-back gentle enough that a
// single starved frame does not visibly shorten the horizon.
constexpr int kShrinkRegionsPerFrame = 2;

// Union AABB of every edit op in a region's log, clamped to that region. The normal edit
// path marks this exact range with generate_probe_misses=true; overflow/repair re-marks must
// replay that same bounded range instead of re-running exact-edit mode across the whole 32^3
// region (which can queue up to kRegionBrickCount bricks per region against max_brick_jobs).
bool exact_edit_aabb(const ve::IVec3 &region, const std::vector<ve::EditOp> &ops,
		ve::IVec3 *lo, ve::IVec3 *hi) {
	const ve::IVec3 r0{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	const ve::IVec3 r1{r0.x + ve::kRegionBricks - 1, r0.y + ve::kRegionBricks - 1,
			r0.z + ve::kRegionBricks - 1};
	bool any = false;
	for (const ve::EditOp &op : ops) {
		ve::IVec3 blo{}, bhi{};
		ve::op_brick_range(op, &blo, &bhi);
		blo.x = std::max(blo.x, r0.x);
		blo.y = std::max(blo.y, r0.y);
		blo.z = std::max(blo.z, r0.z);
		bhi.x = std::min(bhi.x, r1.x);
		bhi.y = std::min(bhi.y, r1.y);
		bhi.z = std::min(bhi.z, r1.z);
		if (blo.x > bhi.x || blo.y > bhi.y || blo.z > bhi.z) continue;
		if (!any) {
			*lo = blo;
			*hi = bhi;
			any = true;
		} else {
			lo->x = std::min(lo->x, blo.x);
			lo->y = std::min(lo->y, blo.y);
			lo->z = std::min(lo->z, blo.z);
			hi->x = std::max(hi->x, bhi.x);
			hi->y = std::max(hi->y, bhi.y);
			hi->z = std::max(hi->z, bhi.z);
		}
	}
	return any;
}

using namespace godot;

void WorldStreamer::initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
		std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
		RegionPass *region_pass, BrickGenPass *brick_gen, std::mutex *occ_mutex,
		std::vector<OccupancyBlock> *occ_inbox, std::atomic<int64_t> *edit_seq,
		const ve::OverrideStore *overrides,
		const std::map<std::tuple<int, int, int>, int> *override_tables) {
	residency_ = residency;
	edit_log_ = edit_log;
	edit_mutex_ = edit_mutex;
	pending_ = pending;
	atlas_ = atlas;
	region_pass_ = region_pass;
	brick_gen_ = brick_gen;
	mesh_ = nullptr;
	overrides_ = overrides;
	override_tables_ = override_tables;
	occ_mutex_ = occ_mutex;
	occ_inbox_ = occ_inbox;
	edit_seq_ = edit_seq;
	overflow_read_.instantiate();
	free_read_.instantiate();
	costs_read_.instantiate();
	for (OccupancyRead &r : occ_reads_) r.read.instantiate();
	// The atlas starts with every slot free and nothing marked, so these are the true values
	// until the first readings land — and they keep cost_by_slot non-null from frame zero,
	// which is what tells RegionResidency the atlas is a priced pool at all.
	cached_free_slots_ = atlas_ ? atlas_->atlas_slot_count() : 0;
	committed_ = 0;
	committed_at_request_ = 0;
	committed_base_ = 0;
	region_slot_costs_.assign(
			static_cast<size_t>(atlas_ ? atlas_->config().max_region_slots : 0), 0);
}

void WorldStreamer::queue_region_regeneration(ve::IVec3 region) {
	if (!edit_mutex_) return;
	std::lock_guard<std::mutex> lock(*edit_mutex_);
	queue_region_regeneration_locked(region);
}

void WorldStreamer::queue_region_regeneration_locked(ve::IVec3 region) {
	if (!edit_mutex_) return;
	forced_regen_.push_back(region);
}

void WorldStreamer::note_marked(ve::IVec3 region, int64_t seq) {
	// One request per region per frame is enough; if the region is marked again later in
	// the same frame with a NEWER op set (load followed by an edit job, say), keep the
	// newest seq so the block that arrives is never stamped older than the mark that
	// produced it.
	for (OccupancyBlock &b : occ_pending_) {
		if (b.region == region) {
			if (seq > b.seq) b.seq = seq;
			return;
		}
	}
	OccupancyBlock b;
	b.region = region;
	b.seq = seq;
	occ_pending_.push_back(std::move(b));
}

void WorldStreamer::pump_occupancy(RenderingDevice *rd) {
	if (!atlas_ || !occ_inbox_ || !occ_mutex_) return;
	// 1. Harvest whatever landed. take_fresh() is true exactly once per arrival.
	for (OccupancyRead &r : occ_reads_) {
		if (!r.active || !r.read->take_fresh()) continue;
		r.active = false;
		if (r.read->data().size() < ve::kOccupancyBlockBytes) {
			// A short read means the copy returned a partial block; if it were dropped the
			// region would stay unknown forever (the mark pass writes a region at most once
			// per frame, and nothing else re-probes it). Re-queue it and try again.
			UtilityFunctions::push_warning("WorldStreamer: short occupancy read for region (",
					r.region.x, ", ", r.region.y, ", ", r.region.z, "): ",
					static_cast<int64_t>(r.read->data().size()), " < ", static_cast<int64_t>(ve::kOccupancyBlockBytes),
					" bytes; re-queueing");
			note_marked(r.region, r.seq);
			continue;
		}
		OccupancyBlock b;
		b.region = r.region;
		b.seq = r.seq;
		b.bytes.assign(r.read->data().ptr(),
				r.read->data().ptr() + ve::kOccupancyBlockBytes);
		std::lock_guard<std::mutex> lock(*occ_mutex_);
		occ_inbox_->push_back(std::move(b));
	}
	// 2. Issue what fits. A region that has since been evicted is skipped: its slot now
	//    describes somewhere else, and region_free has already cleared it.
	for (OccupancyRead &r : occ_reads_) {
		if (r.active || occ_pending_.empty()) continue;
		const OccupancyBlock want = occ_pending_.front();
		occ_pending_.erase(occ_pending_.begin());
		const int slot = residency_->slot_of(want.region);
		if (slot < 0) continue;
		r.region = want.region;
		r.seq = want.seq;
		r.active = r.read->request(rd, atlas_->region_occupancy(),
				static_cast<uint32_t>(slot) * GpuAtlas::occupancy_block_bytes(),
				GpuAtlas::occupancy_block_bytes());
		if (!r.active) {
			// The request did not go out (no device, invalid buffer, API error, ...). The
			// region has no other path back into the grid, so keep it queued and try next
			// frame. Stop issuing this frame: a failed request is device-level (the read was
			// idle), so the other reads would fail identically and every attempt would log.
			UtilityFunctions::push_warning("WorldStreamer: occupancy read request failed for region (",
					want.region.x, ", ", want.region.y, ", ", want.region.z, "); re-queueing");
			note_marked(want.region, want.seq);
			break;
		}
	}
}

void WorldStreamer::drain_readbacks(RenderingDevice *rd) {
	if (!rd) return;
	if (overflow_read_.is_valid()) overflow_read_->drain(rd);
	if (free_read_.is_valid()) free_read_->drain(rd);
	if (costs_read_.is_valid()) costs_read_->drain(rd);
	for (OccupancyRead &r : occ_reads_)
		if (r.read.is_valid()) r.read->drain(rd);
}

void WorldStreamer::harvest_occupancy(RenderingDevice *rd) {
	if (!rd) return;
	drain_readbacks(rd);
	pump_occupancy(rd);
}

int WorldStreamer::run_frame(RenderingDevice *rd, float cx, float cy, float cz) {
	const Clock::time_point t_start = Clock::now();
	last_readback_ms_ = 0.0f;
	last_total_ms_ = 0.0f;
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
	if (!rd || !atlas_ || !residency_ || !edit_log_ || !region_pass_ || !brick_gen_) {
		last_total_ms_ = ms_since(t_start);
		return 0;
	}
	pump_occupancy(rd);

	// The counters are read ASYNCHRONOUSLY: consume whatever a previous frame's request has
	// delivered, then post the next one. The synchronous form blocks the GPU until the copy
	// completes (the engine's own note on buffer_get_data), which made every frame wait for
	// the generation work the last one queued — 39.6 ms on the worst frame measured.
	//
	// The price is that these values are a few frames old, so each is used in the direction
	// that stays safe when it is stale: see the free-slot correction below, and note that the
	// overflow word is only ever CLEARED on a frame whose read actually went out, so no bit
	// can be dropped between a skipped request and the next clear.
	const Clock::time_point t_rb0 = Clock::now();
	uint32_t overflow = 0;
	if (overflow_read_->take_fresh())
		overflow = static_cast<uint32_t>(overflow_read_->as_i32());
	if (free_read_->take_fresh()) {
		cached_free_slots_ = free_read_->as_i32();
		committed_base_ = committed_at_request_;
	}
	if (costs_read_->take_fresh()) {
		const PackedByteArray &b = costs_read_->data();
		const int64_t n = std::min<int64_t>(b.size() / 4, atlas_->config().max_region_slots);
		region_slot_costs_.assign(
				static_cast<size_t>(atlas_->config().max_region_slots), 0);
		if (n > 0) memcpy(region_slot_costs_.data(), b.ptr(), static_cast<size_t>(n) * 4);
	}
	// What the atlas had, less what has been committed to loads since that reading was taken.
	const int free_slots = static_cast<int>(
			std::max<int64_t>(0, cached_free_slots_ - (committed_ - committed_base_)));
	last_readback_ms_ += ms_since(t_rb0);
	overflow_seen_ |= overflow;

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
	// The word is sticky (GpuAtlas::reset_frame_counters). Post the next read FIRST and clear
	// only if it went out: the copy is recorded ahead of the clear in the same command stream,
	// so the bytes in flight are exactly the bits set since the previous clear and none are
	// lost. On a frame where a request is still outstanding, nothing is cleared and the bits
	// simply keep accumulating for the next one — a dropped brick nobody hears about is a
	// hole in the world that never heals, so this arm must never lose a bit.
	const Clock::time_point t_rb1 = Clock::now();
	if (overflow_read_->request(rd, atlas_->frame_counters(), 4, 4)) atlas_->clear_overflow(rd);
	if (free_read_->request(rd, atlas_->counters(), 0, 4)) committed_at_request_ = committed_;
	costs_read_->request(rd, atlas_->region_slot_counts(), 0,
			static_cast<uint32_t>(atlas_->config().max_region_slots) * 4);
	last_readback_ms_ += ms_since(t_rb1);

	// Drain the edit queue. The lock is held for a swap, never across GPU work.
	std::vector<PendingEdit> edits;
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		edits.swap(*pending_);
	}

	// Pace this frame's stream-in against what the atlas can actually pay for. The mark pass
	// tallies the slots each region slot holds; that tally is what a release is worth, and
	// residency spends it: a load may only proceed once releases of regions FURTHER away
	// than the candidate have covered its price. When they cannot, the horizon simply stops
	// arriving — the resident set is capped at what the atlas holds, nearest-first, and no
	// mark ever runs against a free list that cannot serve it.
	//
	// Before this, a load cost a flat guess and was funded by displacing one furthest
	// resident. Both halves were wrong in the same direction: the guess (3072) was ~10x a
	// real region, and the furthest resident is usually pure air holding nothing at all, so
	// the displacement funded nothing. The free list slid to zero and stayed there, and every
	// region streamed in after that had its bricks dropped — sky through freshly loaded
	// ground until the repair sweep caught up.
	int max_region_cost = 0;
	for (int c : region_slot_costs_) max_region_cost = std::max(max_region_cost, c);
	ve::AtlasBudget budget;
	budget.cost_by_slot = region_slot_costs_.data();
	budget.slot_count = static_cast<int>(region_slot_costs_.size());
	// The dearest resident region, not the average: a load has to be priced at what the
	// region it is about to take might cost, and under-pricing is what puts the free list on
	// the floor. Costing a little too much only shortens the horizon slightly.
	budget.per_load = std::max(kSlotsPerRegionFloor,
			max_region_cost > 0 ? max_region_cost : kSlotsPerRegionFallback);
	const int reserve = stream_slot_reserve(atlas_->atlas_slot_count(),
			residency_->config().max_loads_per_frame * budget.per_load);
	budget.available = free_slots - reserve;
	ve::ResidencyPlan plan = residency_->update(cx, cy, cz, budget);
	// Charge this frame's loads against the free count until a fresh reading catches up with
	// them. Evictions are deliberately NOT credited back here: under-counting what is free
	// only shortens the horizon for a frame or two, while over-counting empties the free list.
	committed_ += static_cast<int64_t>(plan.loads.size()) * budget.per_load;
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
			if (!residency_->evict_furthest(cx, cy, cz, &plan, keep, &budget, budget.per_load))
				break;
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
			// Evict until the headroom is REAL: one furthest region is usually air and hands
			// back nothing, which left the edit hitting the same empty free list it was
			// evicting to avoid.
			residency_->evict_furthest(cx, cy, cz, &plan, exclude, &budget, kEditSlotHeadroom);
	}

	// --- buffer_update phase (must precede compute_list_begin: Godot errors otherwise) ---
	// edit_mutex_ guards edit_log_ (voxel_world.h): the tool thread appends ops while this
	// render thread reads, so each ops() read + its pool upload must be atomic. buffer_update
	// is CPU-side recording, not GPU work — microseconds under the lock is fine. The op
	// count is captured here too so the mark loop below needs no second (locked) read.
	struct LoadJob { ve::IVec3 region; int slot; int op_count; int64_t seq; };
	std::vector<LoadJob> load_jobs;
	std::vector<ve::IVec3> forced_regen;
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		forced_regen.swap(forced_regen_);
	}
	// Clear evicted tenants before assigning any reused slot to a load in this same frame.
	// The ordering matters: clearing after the load would erase the replacement table map.
	for (const auto &e : plan.evicts) {
		atlas_->set_region_map_entry(rd, e.map_index, -1);
		atlas_->set_override_table(rd, e.slot, -1, {});
		if (mesh_) mesh_->clear_override_region(e.slot);
	}
	for (const auto &l : plan.loads) {
		int op_count = 0;
		int64_t seq = 0;
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			const std::vector<ve::EditOp> &ops = edit_log_->ops(l.region);
			op_count = static_cast<int>(ops.size());
			// The seq must match the op set just captured, not whatever edit_seq_ says
			// after the mark is recorded: an edit appended between the upload and the
			// mark would be stamped but not in the GPU state. Reading it under the same
			// lock as ops() makes the two a consistent pair.
			seq = edit_seq_ ? edit_seq_->load(std::memory_order_relaxed) : 0;
			// Unconditional, even when empty: the slot may be a recycled eviction still
			// holding the previous tenant's op count, and op_count is the only field the
			// mark pass reads.
			atlas_->upload_region_ops(rd, l.slot, ops.data(), op_count);
		}
		atlas_->set_region_map_entry(rd, l.map_index, l.slot);
		// The table ID and its slot entries are one publication snapshot. Consolidation may
		// replace both under edit_mutex_; reading them in separate critical sections could
		// pair a new table with old entries (or vice versa) for one stream-in.
		int table = -1;
		std::vector<std::pair<int, int>> entries;
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			if (override_tables_) {
				const auto table_it = override_tables_->find(
						std::tuple<int, int, int>{l.region.x, l.region.y, l.region.z});
				if (table_it != override_tables_->end()) table = table_it->second;
			}
			if (overrides_ && table >= 0) {
				const ve::IVec3 base{l.region.x * ve::kRegionBricks,
						l.region.y * ve::kRegionBricks, l.region.z * ve::kRegionBricks};
				for (int z = 0; z < ve::kRegionBricks; z++)
					for (int y = 0; y < ve::kRegionBricks; y++)
						for (int x = 0; x < ve::kRegionBricks; x++) {
							const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
							const int slot = overrides_->slot_of(b);
							if (slot >= 0)
								entries.emplace_back(ve::WorldBounds::brick_index_in_region(b), slot);
						}
			}
		}
		atlas_->set_override_table(rd, l.slot, table, entries);
		if (mesh_ && table >= 0) mesh_->set_override_region(l.region, l.slot, table, entries);
		load_jobs.push_back({l.region, l.slot, op_count, seq});
	}
	// Consolidation has no EditOp left to drive the normal mark path. Force a full-region
	// mark for any resident region explicitly queued by the publication transaction.
	for (const ve::IVec3 region : forced_regen) {
		const int slot = residency_->slot_of(region);
		if (slot < 0) continue;
		bool duplicate = false;
		for (const LoadJob &j : load_jobs) if (j.region == region) { duplicate = true; break; }
		if (duplicate) continue;
		int op_count = 0;
		int64_t seq = 0;
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			op_count = static_cast<int>(edit_log_->ops(region).size());
			seq = edit_seq_ ? edit_seq_->load(std::memory_order_relaxed) : 0;
			atlas_->upload_region_ops(rd, slot, edit_log_->ops(region).data(), op_count);
		}
		load_jobs.push_back({region, slot, op_count, seq});
	}

	// Edits re-mark only the op's brick AABB clamped to each touched region. An op that
	// touches a region's APRON plane is in that region's list by construction (Task 2's
	// one-voxel pad), so the GPU probe — which always uses the brick's OWN region list —
	// sees every op that can change any of its 27 probe points.
	struct EditJob { ve::IVec3 region; int slot; ve::IVec3 lo, hi; int op_count; int64_t seq; };
	std::vector<EditJob> edit_jobs;
	for (const auto &pe : edits) {
		ve::IVec3 blo{}, bhi{};
		ve::op_brick_range(pe.op, &blo, &bhi);
		for (const ve::IVec3 &region : pe.result.touched) {
			const int slot = residency_->slot_of(region);
			if (slot < 0) continue; // off-screen edit: bricks regenerate on stream-in
			// Same race as the load loop: the tool thread may be rebalancing the region
			// map or reallocating this region's list while we memcpy it into the pool.
			// The seq is captured under the same lock as the op count so it cannot run
			// ahead of the data uploaded here.
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
			atlas_->upload_region_ops(rd, slot, ops.data(), static_cast<int>(ops.size()));
			const int64_t seq = edit_seq_ ? edit_seq_->load(std::memory_order_relaxed) : 0;
			const ve::IVec3 r0{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			const EditJob job{region, slot,
					{std::max(blo.x, r0.x), std::max(blo.y, r0.y), std::max(blo.z, r0.z)},
					{std::min(bhi.x, r0.x + 31), std::min(bhi.y, r0.y + 31),
							std::min(bhi.z, r0.z + 31)},
					static_cast<int>(ops.size()), seq};
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
		note_marked(lj.region, lj.seq);
		any = true;
	}
	for (const auto &j : edit_jobs) {
		// mark(force=true) records release, barrier, allocate; the extra barrier BEFORE
		// each job keeps one job's release phase from racing the previous job's allocate
		// phase when two edits share bricks (the race the phase split exists to avoid).
		rd->compute_list_add_barrier(list);
		region_pass_->mark(rd, list, j.region, j.slot, j.lo, j.hi, j.op_count, true, true);
		note_marked(j.region, j.seq);
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
			int64_t seq = 0;
			ve::IVec3 exact_lo{}, exact_hi{};
			bool has_exact = false;
			{
				std::lock_guard<std::mutex> lock(*edit_mutex_);
				const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
				op_count = static_cast<int>(ops.size());
				seq = edit_seq_ ? edit_seq_->load(std::memory_order_relaxed) : 0;
				has_exact = exact_edit_aabb(region, ops, &exact_lo, &exact_hi);
			}
			const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
			rd->compute_list_add_barrier(list);
			if (has_exact) {
				// Keep the full-region force-regen for ordinary dropped/stale surface
				// bricks, then replay only the edit AABB with exact-edit mode so the thin
				// probe-missed result is not undone (and the whole 32^3 region is not
				// re-queued as exact-edit bricks every recovery frame).
				region_pass_->mark(rd, list, region, slot, lo, hi, op_count, true, false);
				rd->compute_list_add_barrier(list);
				region_pass_->mark(rd, list, region, slot, exact_lo, exact_hi, op_count,
						true, true);
			} else {
				// No exact edit range: this is the original plain force-regen recovery.
				// Exact-edit mode would re-queue every brick in the 32^3 region.
				region_pass_->mark(rd, list, region, slot, lo, hi, op_count, true, false);
			}
			note_marked(region, seq);
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
		int64_t seq = 0;
		ve::IVec3 exact_lo{}, exact_hi{};
		bool has_exact = false;
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
			op_count = static_cast<int>(ops.size());
			seq = edit_seq_ ? edit_seq_->load(std::memory_order_relaxed) : 0;
			has_exact = exact_edit_aabb(region, ops, &exact_lo, &exact_hi);
		}
		const ve::IVec3 lo{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
				region.z * ve::kRegionBricks};
		const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
		rd->compute_list_add_barrier(list);
		if (has_exact) {
			// Full-region force-regen first for general dropped/stale bricks; the bounded
			// exact-edit AABB second so probe-missed edits stay resident and generated.
			region_pass_->mark(rd, list, region, slot, lo, hi, op_count, true, false);
			rd->compute_list_add_barrier(list);
			region_pass_->mark(rd, list, region, slot, exact_lo, exact_hi, op_count,
					true, true);
		} else {
			// No exact edit range: plain force-regen only, as in the pre-Round-2 path.
			region_pass_->mark(rd, list, region, slot, lo, hi, op_count, true, false);
		}
		note_marked(region, seq);
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
	last_total_ms_ = ms_since(t_start);
	return static_cast<int>(plan.loads.size() + plan.evicts.size() + edit_jobs.size());
}
