#include "physics/island_manager.h"
#include "voxel_world.h"
#include "mesh/box_merge.h"
#include "render/mesh_service.h"
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/physics_test_motion_parameters3d.hpp>
#include <godot_cpp/classes/physics_test_motion_result3d.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

using namespace godot;

#ifdef DEBUG_ENABLED
#define DBG_LAND(n) (debug_land_.n++)
#else
#define DBG_LAND(n) ((void)0)
#endif

namespace {

using Clock = std::chrono::steady_clock;

// How long a window may wait for the occupancy grid to catch up before the manager gives up
// and runs anyway. The readback ring is eight deep and a request goes out the frame after
// the mark, so a large blast's fan-out lands within a handful of frames; thirty is a
// generous ceiling that keeps a stalled readback from silently disabling destruction.
constexpr int kMaxWindowWaitFrames = 30;
// Frames to cool down after a connectivity pass labels components but cannot submit any
// because a pool is full. This is a backoff, not a guarantee: the run_frame capacity gate
// also keeps a genuinely full pool from relabelling every frame.
constexpr int kRetryCooldownFrames = 30;
// Consecutive failed field extractions originating from one connectivity window before the
// remainder is dropped instead of being re-queued. A full kExtractsPerFrame batch failing
// twice reaches this and stops relabelling a permanently failing window.
constexpr int kMaxExtractFailuresPerWindow = 3;
// Frames to skip a sleeping body after its re-merge paste was rejected. The body is only
// reconsidered once the blocked regions actually have room again; this cooldown prevents a
// per-frame resample/pin/append cycle while the region op lists stay full.
constexpr int kMergeRetryCooldownFrames = 30;
// Extractions submitted per frame. Each is ~1-2 ms on the worker; two keeps a big collapse
// resolving inside a few frames without starving collision meshing.
constexpr int kExtractsPerFrame = 2;
// Spec §5: "components <~0.2 m^3 become plain mesh debris (not raymarch targets)".
constexpr float kDebrisVolumeM3 = 0.2f;
// Every island carve (and crumble) expands its boxes by this much inside the field
// evaluator. A cell-aligned CSG difference leaves SDF == 0 exactly on its faces, and
// every cell-aligned sampler -- the 0.1 m chunk-mesh lattice, the 5 cm brick lattice, the
// 0.4 m occupancy probe -- reads that exact 0 as solid: a razor-thin phantom wall standing
// inside the carved region. Those walls are the "slivers" a freed island wedged against
// (it spawned flush against them and slept mid-air for ever), and they are also what kept
// carved cells reading solid to the next connectivity pass. Two centimetres is far below
// the 5 cm render lattice, so the planes read as air everywhere while the island's own
// collision boxes and the connectivity bookkeeping stay cell-exact.
constexpr float kCarveClearanceM = 0.02f;
// A sleeping body is only re-merged once it is actually resting on the terrain, not while
// it is still airborne. The gate compares the compound's lowest box corner with the
// highest terrain hit under its XZ footprint (start_merges), so a few metres of clearance
// covers uneven ground without letting a floating sleeper paste rubble into the sky
// (which would immediately be re-extracted).
constexpr float kMergeGroundClearanceM = 2.0f;
// Tolerance for treating a sleeping body's current transform as the same rest pose it was
// resampled from. A sleeping body may still jitter by fractions of a millimetre; anything at
// or above a centimetre (or a visibly changed basis) means the resample is stale.
constexpr float kMergePosePositionEps = 0.01f;
constexpr float kMergePoseBasisEps = 0.02f;

bool same_rest_pose(const Transform3D &a, const Transform3D &b) {
	if (a.origin.distance_to(b.origin) > kMergePosePositionEps) return false;
	for (int i = 0; i < 3; i++)
		if (a.basis[i].distance_to(b.basis[i]) > kMergePoseBasisEps) return false;
	return true;
}

// The residency's view of the world field, for ve::refine_anchoring. The lock is taken per
// call rather than held, exactly as ColliderStreamer::LogProbe does, so an edit landing
// mid-refinement waits rather than deadlocks.
struct LogContactProbe : ve::ContactProbe {
	const ve::Generator *gen = nullptr;
	ve::EditLog *log = nullptr;
	std::mutex *mu = nullptr;
	const ve::VolumeStore *volumes = nullptr;
	int face_samples = 9;

	int contact_samples(ve::IVec3 cell, int axis) const override {
		std::lock_guard<std::mutex> lock(*mu);
		const std::vector<ve::EditOp> &ops = log->ops(ve::WorldBounds::region_of_brick(cell));
		return ve::contact_samples_field(*gen, ops.data(), static_cast<int>(ops.size()), cell,
				axis, face_samples, volumes);
	}
};

} // namespace

IslandManager::~IslandManager() {
	teardown();
}

void IslandManager::initialize(VoxelWorld *world) {
	teardown();
	world_ = world;
	atlas_used_.assign(kMaxIslands, 0);
	next_id_ = 1;
	next_window_id_ = 1;
	slot_high_water_ = 0;
	connectivity_runs_ = 0;
	islands_spawned_ = 0;
	debris_spawned_ = 0;
	islands_merged_ = 0;
	refused_ = 0;
	last_ms_ = 0.0f;
}

void IslandManager::teardown() {
	for (IslandBody *b : bodies_) {
		if (!b) continue;
		if (world_) world_->release_volume_slot(b->info().volume_slot);
		delete b;
	}
	bodies_.clear();
	for (const InFlight &f : in_flight_)
		if (world_) world_->release_volume_slot(f.volume_slot);
	in_flight_.clear();
	for (const Merging &m : merging_)
		if (world_) world_->release_volume_slot(m.out_slot);
	merging_.clear();
	merge_retries_.clear();
	{
		std::lock_guard<std::mutex> lock(windows_mutex_);
		windows_.clear();
	}
	atlas_used_.clear();
	world_ = nullptr;
}

void IslandManager::note_edit(const ve::EditOp &op, int64_t seq) {
	if (op.type == ve::kOpSpherePaint) return; // paint moves no matter
	std::lock_guard<std::mutex> lock(windows_mutex_);
	float lo[3], hi[3];
	ve::op_world_aabb(op, lo, hi);
	PendingWindow w;
	w.id = next_window_id_++;
	const auto cell = [](float v) {
		return static_cast<int>(std::floor(v / ve::kOccupancyCellSize));
	};
	w.lo = {cell(lo[0]), cell(lo[1]), cell(lo[2])};
	w.hi = {cell(hi[0]), cell(hi[1]), cell(hi[2])};
	w.seq = seq;
	// Spec §5's "explosion + radial impulse": a piece freed by a blast is thrown away from
	// it, which is the difference between rubble falling and rubble erupting.
	if (op.type == ve::kOpSphereSubtract) {
		for (int a = 0; a < 3; a++) w.impulse_from[a] = op.pos[a];
		w.impulse_scale = op.radius;
	}

	const auto overlaps = [](const PendingWindow &a, const PendingWindow &b) {
		return a.lo.x <= b.hi.x && a.hi.x >= b.lo.x && a.lo.y <= b.hi.y &&
				a.hi.y >= b.lo.y && a.lo.z <= b.hi.z && a.hi.z >= b.lo.z;
	};
	const auto absorb = [](PendingWindow &dst, const PendingWindow &src) {
		dst.lo = {std::min(dst.lo.x, src.lo.x), std::min(dst.lo.y, src.lo.y),
				std::min(dst.lo.z, src.lo.z)};
		dst.hi = {std::max(dst.hi.x, src.hi.x), std::max(dst.hi.y, src.hi.y),
				std::max(dst.hi.z, src.hi.z)};
		dst.seq = std::max(dst.seq, src.seq);
		if (src.impulse_scale > dst.impulse_scale) {
			dst.impulse_scale = src.impulse_scale;
			for (int a = 0; a < 3; a++) dst.impulse_from[a] = src.impulse_from[a];
		}
	};

	// Merge into every overlapping window rather than queueing a second one: spec §5 wants ONE
	// connectivity run per frame however many blasts landed, and two windows over the same
	// rubble would label the same component twice. A single edit can overlap several existing
	// windows (for example when it bridges two previously disjoint pending edits), so all of
	// them absorb it.
	bool merged = false;
	for (PendingWindow &e : windows_) {
		if (!overlaps(e, w)) continue;
		merged = true;
		absorb(e, w);
	}
	if (!merged) windows_.push_back(w);

	// A bridging edit can also make two previously disjoint windows overlap each other.
	// Coalesce until stable so two windows never cover the same component.
	bool coalesced = true;
	while (coalesced) {
		coalesced = false;
		for (auto it = windows_.begin(); it != windows_.end(); ++it) {
			auto jt = it;
			++jt;
			while (jt != windows_.end()) {
				if (!overlaps(*it, *jt)) {
					++jt;
					continue;
				}
				absorb(*it, *jt);
				jt = windows_.erase(jt);
				coalesced = true;
			}
		}
	}
}

bool IslandManager::window_is_fresh(const PendingWindow &w) const {
	// Every region the window touches must have been re-probed since the edit. A region with
	// no block at all is "unknown", which the flood treats as anchored ground -- it cannot
	// hide an island, so it does not hold the window up either.
	const ve::OccupancyGrid &grid = world_->occupancy();
	const ve::IVec3 rlo = ve::WorldBounds::region_of_brick(w.lo);
	const ve::IVec3 rhi = ve::WorldBounds::region_of_brick(w.hi);
	for (int z = rlo.z; z <= rhi.z; z++)
		for (int y = rlo.y; y <= rhi.y; y++)
			for (int x = rlo.x; x <= rhi.x; x++) {
				const int64_t s = grid.block_seq({x, y, z});
				if (s >= 0 && s < w.seq) return false;
			}
	return true;
}

int IslandManager::run_connectivity(const PendingWindow &pw) {
	if (gen_ == nullptr) return 0; // not initialized: no field, no connectivity
	connectivity_runs_++;
	ve::FloodWindow w = ve::FloodWindow::around(pw.lo, pw.hi, ve::kFloodWindowCells);
	ve::LinkCuts cuts;
	ve::FloodResult r;
	LogContactProbe probe;
	probe.gen = gen_;
	probe.log = world_->edit_log();
	probe.mu = &world_->edit_mutex();
	probe.volumes = &world_->volumes();
	probe.face_samples = refine_cfg_.face_samples;

	for (int expand = 0;; expand++) {
		ve::flood_anchored(world_->occupancy(), w, &cuts, &r);
		// Spec §5's marginal-contact refinement, before labelling: a piece held by one thin
		// neck must be cut loose BEFORE the labeller decides it is anchored.
		ve::refine_anchoring(world_->occupancy(), probe, refine_cfg_, &cuts, &r);
		if (!r.frontier_reached || expand >= ve::kMaxWindowExpansions) break;
		// Spec §5: "expanding if the frontier is reached".
		w = ve::FloodWindow::around(pw.lo, pw.hi, w.dim * 2);
		cuts.clear();
	}
	if (r.frontier_reached)
		UtilityFunctions::print_verbose(
				"IslandManager: a loose piece reaches the widest window; treating it as anchored");

	std::vector<ve::IslandComponent> comps;
	ve::label_islands(r, comp_cfg_, &comps);
	components_labelled_ += static_cast<int>(comps.size());

	if (!world_->mesh_service()->extraction_available()) {
		// Permanent for this physics lifetime: every field extraction would fail because the
		// worker has no IslandExtractPass. Do not allocate volume slots, submit jobs, or
		// re-queue a remainder that can only fail forever. The components stay attached in
		// the field, which is the same fail-soft outcome the worker would produce.
		refused_ += static_cast<int>(comps.size());
		refused_unavailable_ += static_cast<int>(comps.size());
		return 0;
	}

	int submitted = 0;
	bool transient_refusal = false;
	std::vector<IslandExtractJob> jobs;
	for (const ve::IslandComponent &c : comps) {
		if (submitted >= kExtractsPerFrame) break;
		if (live_body_count() + static_cast<int>(in_flight_.size()) >= max_dynamic_bodies_) {
			refused_++;
			refused_body_cap_++;
			transient_refusal = true;
			break;
		}

		std::vector<ve::CellBox> boxes;
		if (!ve::greedy_box_merge(c.cells, ve::kMaxIslandBoxes, &boxes)) {
			// Fail-soft (spec §8): a shape too fragmented for 64 boxes stays attached. The
			// labeller's extent bound does not bound box COUNT, and a partial carve would
			// leave matter in two places at once.
			refused_++;
			refused_box_merge_++;
			continue;
		}
		float wlo[3], whi[3];
		c.world_aabb(wlo, whi);
		IslandExtractJob job;
		job.kind = kExtractField;
		job.id = next_id_++;
		job.boxes = boxes;
		job.dim = ve::kIslandDim;
		if (!ve::plan_island_lattice(wlo, whi, job.dim, &job.voxel, job.origin)) {
			refused_++;
			refused_lattice_++;
			continue;
		}
		{
			std::lock_guard<std::mutex> lock(world_->edit_mutex());
			if (!world_->edit_log()) {
				refused_++;
				continue;
			}
			ve::collect_ops_for_aabb(*world_->edit_log(), wlo, whi, &job.ops);
			float lattice_hi[3] = {job.origin[0] + (job.dim - 1) * job.voxel, job.origin[1] + (job.dim - 1) * job.voxel, job.origin[2] + (job.dim - 1) * job.voxel};
			ve::IVec3 blo = ve::WorldBounds::brick_of_point(job.origin[0], job.origin[1], job.origin[2]);
			ve::IVec3 bhi = ve::WorldBounds::brick_of_point(lattice_hi[0], lattice_hi[1], lattice_hi[2]);
			if (!world_->snapshot_field_sources(job.ops, blo, bhi, &job.snapshot)) {
				refused_++;
				refused_op_cap_++;
				continue;
			}
		}
		job.gen = gen_;
		job.override_table = world_->override_table_for_region(
				ve::WorldBounds::region_of_point(job.origin[0], job.origin[1], job.origin[2]));
		// Refuse before allocating a volume slot or submitting: the extraction pass cannot
		// evaluate more than kMaxRegionOps ops, so this component can never be carved by the
		// current field/worker limits. Fail-soft leaves it attached.
		if (job.ops.size() > static_cast<size_t>(ve::kMaxRegionOps)) {
			refused_++;
			refused_op_cap_++;
			continue;
		}
		const int slot = world_->volumes().allocate();
		if (slot < 0) {
			refused_++;
			refused_pool_full_++;
			transient_refusal = true;
			continue; // pool full: leave it attached
		}

		InFlight f;
		f.id = job.id;
		f.boxes = boxes;
		f.volume_slot = slot;
		f.voxel = job.voxel;
		f.dim = job.dim;
		f.window = pw;
		f.ops = job.ops;
		for (int a = 0; a < 3; a++) {
			f.origin[a] = job.origin[a];
			f.aabb_lo[a] = wlo[a];
			f.aabb_hi[a] = whi[a];
		}
		if (pw.impulse_scale > 0.0f) {
			// Away from the blast, falling off with distance, scaled by the blast radius.
			const float cx = 0.5f * (wlo[0] + whi[0]);
			const float cy = 0.5f * (wlo[1] + whi[1]);
			const float cz = 0.5f * (wlo[2] + whi[2]);
			float v[3] = {cx - pw.impulse_from[0], cy - pw.impulse_from[1],
					cz - pw.impulse_from[2]};
			const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
			if (len > 0.001f) {
				const float mag = 60.0f * pw.impulse_scale /
						std::max(len, pw.impulse_scale);
				for (int a = 0; a < 3; a++) f.impulse[a] = v[a] / len * mag;
			}
		}
		in_flight_.push_back(std::move(f));
		jobs.push_back(std::move(job));
		submitted++;
	}
	// A blast can label more loose components than kExtractsPerFrame (or than the body cap
	// has room for). Re-queue the same window so the next connectivity pass -- gated on no
	// in-flight extractions -- continues with the remainder once the current batch has been
	// carved.
	if (submitted > 0 && submitted < static_cast<int>(comps.size())) {
		std::lock_guard<std::mutex> lock(windows_mutex_);
		windows_.push_back(pw);
	} else if (submitted == 0 && transient_refusal) {
		// Zero-progress but transiently refused (body cap or volume pool full): keep the
		// window queued with a cooldown. A genuinely full pool would otherwise relabel every
		// frame; the run_frame capacity gate below also skips while there is no room.
		std::lock_guard<std::mutex> lock(windows_mutex_);
		PendingWindow retry = pw;
		retry.retry_cooldown = kRetryCooldownFrames;
		windows_.push_back(retry);
	}
	// Permanent failures (unmergeable shape, unplannable lattice, etc.) are deliberately
	// dropped: no amount of retrying will make them progress.
	if (!jobs.empty() && !world_->mesh_service()->submit_extracts(std::move(jobs))) {
		// A rejected submit must not strand the InFlight entries we just pushed: no result
		// will ever arrive for them, so they would leak volume slots and the run_frame gate
		// would disable connectivity forever. Mirror start_merges() by rolling the entries
		// back and keeping the originating window alive for a later retry.
		for (int i = 0; i < submitted; i++) {
			const InFlight &f = in_flight_.back();
			world_->release_volume_slot(f.volume_slot);
			in_flight_.pop_back();
		}
		queue_retry_window(pw);
		return 0;
	}
	return submitted;
}

void IslandManager::queue_retry_window(const PendingWindow &w) {
	std::lock_guard<std::mutex> lock(windows_mutex_);
	PendingWindow retry = w;
	retry.retry_cooldown = kRetryCooldownFrames;
	// Several extractions from one connectivity window can land in the same frame. If all of
	// them hit a full atlas before any carve lands, they must not each push another copy of
	// the same originating window: that would only cause redundant connectivity runs.
	for (PendingWindow &e : windows_) {
		if (e.id == retry.id) {
			// A remainder window is already queued without a cooldown (it is simply the
			// unsubmitted tail of an earlier connectivity pass). If one of its extractions
			// lands against a full atlas, the retry backoff must be applied to that existing
			// entry too, or the next frame would relabel the same window immediately.
			e.retry_cooldown = kRetryCooldownFrames;
			return;
		}
	}
	windows_.push_back(retry);
}

void IslandManager::note_extract_failure(const PendingWindow &w) {
	std::lock_guard<std::mutex> lock(windows_mutex_);
	for (auto it = windows_.begin(); it != windows_.end(); ++it) {
		if (it->id != w.id) continue;
		it->extract_failures++;
		if (it->extract_failures >= kMaxExtractFailuresPerWindow) {
			// A full submitted batch has failed repeatedly; the pass is not going to recover
			// for this window. Drop the remainder instead of relabelling/resubmitting it
			// forever. The components stay attached in the field, which is the same fail-soft
			// outcome the worker itself would produce.
			windows_.erase(it);
		} else {
			it->retry_cooldown = kRetryCooldownFrames;
		}
		return;
	}
	// No queued remainder: this was a fully-submitted window (or the remaining window was
	// dropped by an earlier failure). Do not silently lose the edit. Re-queue it so the same
	// backoff/drop policy applies; carry the previous failure count so a window that has
	// already failed once reaches the drop threshold on the next failure instead of resetting.
	const int failures = w.extract_failures + 1;
	if (failures >= kMaxExtractFailuresPerWindow) return;
	PendingWindow retry = w;
	retry.retry_cooldown = kRetryCooldownFrames;
	retry.extract_failures = failures;
	windows_.push_back(retry);
}

void IslandManager::note_extract_success(const PendingWindow &w) {
	std::lock_guard<std::mutex> lock(windows_mutex_);
	for (PendingWindow &e : windows_) {
		if (e.id == w.id) {
			e.extract_failures = 0;
			return;
		}
	}
}

void IslandManager::note_merge_rejected(int body_index, const ve::EditLog::AppendResult &paste) {
	auto it = std::find_if(merge_retries_.begin(), merge_retries_.end(),
			[&](const MergeRetry &r) { return r.body_index == body_index; });
	if (it == merge_retries_.end()) {
		merge_retries_.push_back(
				MergeRetry{body_index, kMergeRetryCooldownFrames, paste.rejected, false});
		return;
	}
	it->cooldown = kMergeRetryCooldownFrames;
	for (const ve::IVec3 &region : paste.rejected)
		if (std::find(it->blocked_regions.begin(), it->blocked_regions.end(), region) ==
				it->blocked_regions.end())
			it->blocked_regions.push_back(region);
}

void IslandManager::block_merge_permanently(int body_index) {
	auto it = std::find_if(merge_retries_.begin(), merge_retries_.end(),
			[&](const MergeRetry &r) { return r.body_index == body_index; });
	if (it == merge_retries_.end()) {
		merge_retries_.push_back(MergeRetry{body_index, 0, {}, true});
		return;
	}
	it->permanent = true;
	it->cooldown = 0;
}

bool IslandManager::merge_retry_blocked(int body_index) {
	// op_count() reads EditLog's region lists, which append_edit/note_edit mutate under
	// edit_mutex_; a tool-thread edit can land while start_merges() is checking retries.
	std::lock_guard<std::mutex> lock(world_->edit_mutex());
	for (auto it = merge_retries_.begin(); it != merge_retries_.end();) {
		if (it->body_index != body_index) {
			++it;
			continue;
		}
		if (it->cooldown > 0 || it->permanent) return true;
		bool still_full = false;
		for (const ve::IVec3 &region : it->blocked_regions) {
			if (world_->edit_log()->op_count(region) >= ve::kMaxRegionOps) {
				still_full = true;
				break;
			}
		}
		if (still_full) {
			// The region that rejected the paste is still full; keep this body out of
			// start_merges() instead of burning a resample/pin/append attempt every frame.
			it->cooldown = kMergeRetryCooldownFrames;
			return true;
		}
		it = merge_retries_.erase(it);
		return false;
	}
	return false;
}

// Remove a loose component the extractor could not represent. Returns false when it must be
// left alone: the cells are already air (a carve landed while this extraction was in flight,
// so carving again would spend permanent region ops on nothing), a cell is kCellFull (no air
// sample at 5 cm -- a disagreement that size is not a thin sheet and carving would delete
// solid rock), or a region has no op headroom for the boxes.
bool IslandManager::crumble_component(const InFlight &f) {
	if (f.boxes.empty()) return false;
	std::lock_guard<std::mutex> lock(world_->edit_mutex());
	if (!world_->edit_log()) return false;

	const ve::OccupancyGrid &grid = world_->occupancy();
	bool any_solid = false;
	for (const ve::CellBox &box : f.boxes)
		for (int z = box.lo.z; z <= box.hi.z; z++)
			for (int y = box.lo.y; y <= box.hi.y; y++)
				for (int x = box.lo.x; x <= box.hi.x; x++) {
					const ve::CellState state = grid.state({x, y, z});
					if (state == ve::kCellFull) return false;
					if (state != ve::kCellAir) any_solid = true;
				}
	if (!any_solid) return false;

	// Headroom region by region before the first append: a half-applied carve would leave
	// half the sheet standing and spend the ops anyway.
	std::vector<ve::IVec3> regions;
	std::vector<int> ops_here;
	for (const ve::CellBox &box : f.boxes) {
		ve::IVec3 rlo, rhi;
		ve::op_region_range(ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM), &rlo, &rhi);
		for (int z = rlo.z; z <= rhi.z; z++)
			for (int y = rlo.y; y <= rhi.y; y++)
				for (int x = rlo.x; x <= rhi.x; x++) {
					const ve::IVec3 region{x, y, z};
					const auto it = std::find(regions.begin(), regions.end(), region);
					if (it == regions.end()) {
						regions.push_back(region);
						ops_here.push_back(1);
					} else {
						ops_here[static_cast<size_t>(it - regions.begin())]++;
					}
				}
	}
	for (size_t i = 0; i < regions.size(); i++)
		if (world_->edit_log()->op_count(regions[i]) + ops_here[i] > ve::kMaxRegionOps)
			return false;

	// notify_islands = false: this matter was already labelled unanchored, so removing it
	// cannot loosen anything that was not loose already, and a window per crumble would put
	// the connectivity pass back into the loop this function exists to break.
	for (const ve::CellBox &box : f.boxes)
		world_->append_edit_locked(
				ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM), false);
	// Tell the occupancy grid straight away, exactly as the spawning carve does: the GPU
	// readback that would say the same thing is several frames out, and until it lands the
	// next connectivity run would label this component all over again.
	for (const ve::CellBox &box : f.boxes)
		for (int z = box.lo.z; z <= box.hi.z; z++)
			for (int y = box.lo.y; y <= box.hi.y; y++)
				for (int x = box.lo.x; x <= box.hi.x; x++)
					world_->occupancy().set_cell({x, y, z}, ve::kCellAir, world_->edit_seq());
	crumbled_++;
	return true;
}

void IslandManager::land_extraction(const IslandExtractResult &r) {
	auto it = std::find_if(in_flight_.begin(), in_flight_.end(),
			[&r](const InFlight &f) { return f.id == r.id; });
	if (it == in_flight_.end()) return;
	const InFlight f = *it;
	in_flight_.erase(it);
	bool empty = r.data.solid_voxels == 0;
	if (debug_empty_next_extraction_) {
		debug_empty_next_extraction_ = false;
		empty = true;
	}
	if (r.failed || empty) {
		world_->release_volume_slot(f.volume_slot);
		if (r.failed) {
			note_extract_failure(f.window);
			return;
		}
		note_extract_success(f.window);
		// NOT "nothing there after all". The occupancy grid the labeller reads is a
		// CONSERVATIVE test over the 5 cm brick lattice, while the island lattice is a point
		// sample at up to 10 cm -- ve::plan_island_lattice drops to the coarse pitch for any
		// component wider than 2.95 m. A sheet thinner than that pitch therefore reads SOLID
		// to the labeller and EMPTY to the extractor, and a sphere carve through a sphere-add
		// pillar leaves exactly that: paper-thin dishes a few centimetres thick.
		//
		// Leaving one standing is the worst of the three outcomes. The matter stays as static
		// terrain inside the space the freed piece was cut out of, so the piece wedges against
		// it instead of falling; and because nothing changed, every later connectivity run
		// labels the same cells, plans the same lattice and runs the same extraction again,
		// for ever. Sub-voxel debris crumbles instead: carve the cells, spawn nothing.
		if (!crumble_component(f)) {
			refused_++;
			refused_empty_++;
		}
		return;
	}
	// A successful extraction is progress on this window; reset any prior failure streak so
	// only persistently failing batches accumulate toward dropping the remainder.
	note_extract_success(f.window);

	// The extraction was computed from the field as of submit time. If a newer edit (including
	// another carve from the same batch) has changed any op that can influence this component's
	// AABB, the volume in hand is stale: carving it into the current field would remove matter
	// using a shape that no longer matches the field. The freshness comparison is deliberately
	// re-run INSIDE the edit_mutex_ hold that also does preflight/pin/spawn/carve, so no
	// tool-thread edit can land between the comparison and the first carve.

	const float solid_m3 = static_cast<float>(r.data.solid_voxels) * f.voxel * f.voxel * f.voxel;
	const bool debris = solid_m3 < kDebrisVolumeM3;
	int atlas_slot = -1;
	if (!debris) {
		atlas_slot = free_atlas_slot();
		if (atlas_slot < 0) {
			// Spec §5's "<=32 island bodies (oldest sleepers merge early)". Nothing is
			// carved yet, so refusing costs only that this piece stays put for now. Unlike
			// refusals before submission, the originating window has already been popped by
			// the time we learn the atlas is full, so re-queue it or the edit is lost when a
			// slot later frees.
			refused_++; DBG_LAND(atlas_full);
			world_->release_volume_slot(f.volume_slot);
			queue_retry_window(f.window);
			return;
		}
		atlas_used_[static_cast<size_t>(atlas_slot)] = 1;
		const int high = std::max(slot_high_water_.load(std::memory_order_relaxed), atlas_slot + 1);
		slot_high_water_.store(high, std::memory_order_relaxed);
	}

	if (!world_->volumes().store(f.volume_slot, r.data)) {
		if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
		world_->release_volume_slot(f.volume_slot);
		refused_++; DBG_LAND(store_failed);
		return; // nothing carved yet: the piece stays attached in the field
	}

	// Before any carve lands, verify every region the carve (and the later restore volume-add)
	// will touch has enough op-list headroom. A region at kMaxRegionOps - 1 would accept the
	// carve, hit 256, and then reject the restore; the old abort paths turned that into a
	// release-build crash. Refuse up front: the component stays attached, which is the safe
	// fail-soft direction.
	//
	// The preflight, the carve loop, and any restore all run under one edit_mutex_ hold. A
	// tool-thread append_edit cannot fill a region between the preflight and the restore, so
	// a preflight pass means every carve box will be accepted and the restore volume-add will
	// be accepted if a spawn failure later needs it.
	const auto has_restore_headroom = [&]() -> bool {
		std::vector<ve::IVec3> carve_regions;
		std::vector<ve::IVec3> restore_regions;
		const auto add_region = [&](std::vector<ve::IVec3> &out, ve::IVec3 r) {
			if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
		};
		for (const ve::CellBox &box : f.boxes) {
			ve::IVec3 rlo, rhi;
			ve::op_region_range(ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM),
					&rlo, &rhi);
			for (int z = rlo.z; z <= rhi.z; z++)
				for (int y = rlo.y; y <= rhi.y; y++)
					for (int x = rlo.x; x <= rhi.x; x++)
						add_region(carve_regions, {x, y, z});
		}
		ve::IVec3 rlo, rhi;
		ve::op_region_range(ve::make_volume_add(f.volume_slot, f.origin, f.voxel, f.dim),
				&rlo, &rhi);
		for (int z = rlo.z; z <= rhi.z; z++)
			for (int y = rlo.y; y <= rhi.y; y++)
				for (int x = rlo.x; x <= rhi.x; x++)
					add_region(restore_regions, {x, y, z});
		for (const ve::IVec3 &region : carve_regions)
			if (std::find(restore_regions.begin(), restore_regions.end(), region) ==
					restore_regions.end())
				return false;
		for (const ve::IVec3 &region : restore_regions) {
			int carve_ops_here = 0;
			for (const ve::CellBox &box : f.boxes) {
				ve::IVec3 brlo, brhi;
				ve::op_region_range(ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM),
						&brlo, &brhi);
				if (region.x >= brlo.x && region.x <= brhi.x &&
						region.y >= brlo.y && region.y <= brhi.y &&
						region.z >= brlo.z && region.z <= brhi.z)
					carve_ops_here++;
			}
			if (world_->edit_log()->op_count(region) + carve_ops_here + 1 >
					ve::kMaxRegionOps)
				return false;
		}
		return true;
	};

	IslandBody *b = nullptr;
	const Ref<World3D> w3 = world_->get_world_3d();
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		if (!world_->edit_log()) {
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			world_->release_volume_slot(f.volume_slot);
			refused_++; DBG_LAND(no_edit_log);
			return;
		}
		if (!has_restore_headroom()) {
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			world_->release_volume_slot(f.volume_slot);
			refused_++; DBG_LAND(preflight);
			return; // preflight refused: no carve, no hole, the component stays attached
		}

		// Re-check freshness under the SAME lock as preflight/pin/spawn/carve. The ops were
		// captured at submit time; if the field changed since then, do not carve a stale
		// volume. Release the atlas/volume resources and back off -- the edit that changed the
		// field queued its own connectivity window.
		//
		// Only ops that reach INSIDE THE BOXES count. The captured list was gathered over the
		// component's whole AABB, but the island is the field intersected with the box union
		// (ve::extract_island_volume masks every sample with max(field, box union)), so an op
		// that misses every box changed nothing this extraction depends on and nothing the
		// carve is about to remove.
		//
		// Comparing the whole AABB list instead was a livelock. One connectivity pass submits
		// kExtractsPerFrame extractions from the SAME blast, and neighbouring components have
		// overlapping AABBs; the first to land appends its carve ops, which land in the second
		// one's AABB and declare it stale even though the two components are cell-disjoint by
		// construction (ve::label_islands emits disjoint components and ve::greedy_box_merge
		// tiles each one exactly). The second was thrown away and its window re-queued, every
		// time -- so a blast that freed many pieces dropped one piece per pass at best, and
		// could make no progress at all. That is the "some parts take a very long time to fall,
		// or never do" report. Overlap is STRICT so that a sibling's box sharing a face plane
		// with ours -- which removes nothing on our side of it -- does not count.
		{
			const auto reaches_the_boxes = [&f](const ve::EditOp &op) {
				float olo[3], ohi[3];
				ve::op_world_aabb(op, olo, ohi);
				for (const ve::CellBox &box : f.boxes) {
					float blo[3], bhi[3];
					box.world_aabb(blo, bhi);
					bool overlaps = true;
					for (int a = 0; a < 3; a++)
						if (!(olo[a] < bhi[a] && ohi[a] > blo[a])) {
							overlaps = false;
							break;
						}
					if (overlaps) return true;
				}
				return false;
			};
			std::vector<ve::EditOp> current_ops;
			ve::collect_ops_for_aabb(*world_->edit_log(), f.aabb_lo, f.aabb_hi, &current_ops);
			std::vector<ve::EditOp> now, then;
			for (const ve::EditOp &op : current_ops)
				if (reaches_the_boxes(op)) now.push_back(op);
			for (const ve::EditOp &op : f.ops)
				if (reaches_the_boxes(op)) then.push_back(op);
			const bool stale = now.size() != then.size() ||
					!std::equal(now.begin(), now.end(), then.begin(),
							[](const ve::EditOp &a, const ve::EditOp &b) {
								return std::memcmp(&a, &b, sizeof(ve::EditOp)) == 0;
							});
			if (stale) {
				if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
				world_->release_volume_slot(f.volume_slot);
				queue_retry_window(f.window);
				refused_++; DBG_LAND(stale);
				return; // stale extraction: no carve, the component stays attached
			}
		}

		// Pin the birth volume before the first carve. The slot is already stored; pinning
		// here means every later restore path can reference it, and a pin failure happens
		// before any hole exists, so the stored slot can still be released.
		if (!world_->volumes().pin(f.volume_slot)) {
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			world_->release_volume_slot(f.volume_slot);
			refused_++; DBG_LAND(pin_failed);
			return; // no carve happened: the component stays attached
		}

		IslandSpawn info;
		info.volume_slot = f.volume_slot;
		info.atlas_slot = atlas_slot;
		info.boxes = f.boxes;
		for (int a = 0; a < 3; a++) info.lattice_origin[a] = f.origin[a];
		info.voxel = f.voxel;
		info.dim = f.dim;
		info.solid_voxels = r.data.solid_voxels;
		for (int a = 0; a < 3; a++) info.impulse[a] = f.impulse[a];
		info.debris = debris;

		bool restore_referenced_slot = false;
		const auto release_unreferenced_birth_slot = [&]() {
			world_->volumes().unpin(f.volume_slot);
			world_->release_volume_slot(f.volume_slot);
		};

		// 1. Spawn (spec §5 step 3, reordered). A live body must exist before any carve is
		//    committed: then a carve can never return with neither a body nor a full restore.
		//    The body is created at its final position; it is not added to the manager's body
		//    list until the carve outcome is known, so the restore-and-despawn paths do not
		//    need to unwind a published body.
		IslandBody *body = new IslandBody();
		const bool spawn_failed = debug_fail_next_spawn_ ||
				!body->spawn(w3.is_valid() ? w3->get_space() : RID(),
						w3.is_valid() ? w3->get_scenario() : RID(), info, &r.data);
		debug_fail_next_spawn_ = false;
		if (spawn_failed) {
			delete body;
			debug_fail_next_carve_ = false;
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			release_unreferenced_birth_slot();
			refused_++; DBG_LAND(spawn_failed);
			return; // no carve happened: the component stays attached
		}

		// 2. Carve (spec §5 step 1). The boxes tile the component exactly, so this removes the
		//    material that just became a body and nothing else. Ordered after the spawn so a
		//    live body is already in place before any rock is removed from the field.
		bool carve_rejected = false;
		std::vector<ve::IVec3> carved_regions;
		for (const ve::CellBox &box : f.boxes) {
			const ve::EditLog::AppendResult carve = world_->append_edit_locked(
					ve::make_box_subtract(box.lo, box.hi, kCarveClearanceM));
			for (const ve::IVec3 &region : carve.touched) carved_regions.push_back(region);
			if (debug_fail_next_carve_) {
				debug_fail_next_carve_ = false;
				carve_rejected = true;
				break;
			}
			if (!carve.rejected.empty()) {
				carve_rejected = true;
				break;
			}
		}
		debug_fail_next_carve_ = false;

		if (!carve_rejected) {
			// The carve is fully committed and the body is already live. Tell the occupancy
			// grid straight away; the GPU readback that would say the same thing is several
			// frames out, and until it lands the next connectivity run would find this
			// component all over again and carve it twice.
			for (const ve::CellBox &box : f.boxes)
				for (int z = box.lo.z; z <= box.hi.z; z++)
					for (int y = box.lo.y; y <= box.hi.y; y++)
						for (int x = box.lo.x; x <= box.hi.x; x++)
							world_->occupancy().set_cell(
									{x, y, z}, ve::kCellAir, world_->edit_seq());
#ifdef DEBUG_ENABLED
			for (const ve::CellBox &box : f.boxes) debug_carved_boxes_.push_back(box);
#endif
			// A live body's birth volume is normally unpinned. If a carve-rejection restore
			// already appended a volume-add naming this slot, it must stay pinned forever.
			if (!restore_referenced_slot) world_->volumes().unpin(f.volume_slot);
			b = body;
		} else {
			// With the preflight and the carve under the same lock this is unreachable under
			// op-cap pressure. It remains as a defensive fail-soft branch: a carve was
			// rejected (possibly after some boxes were already removed). A live body already
			// exists, so the only question is whether the field can be fully restored and the
			// body despawned, or whether the body must stay to fill every carved cell.
			if (carved_regions.empty()) {
				// Nothing was carved (e.g. an out-of-bounds edge case), so there is no hole to
				// restore. The body is redundant; the stored birth slot is unreferenced and can
				// be released.
				delete body;
				if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
				release_unreferenced_birth_slot();
				refused_++; DBG_LAND(carve_nothing);
				return; // nothing was carved: the component simply stays attached
			}
			// The slot was pinned before the first carve, so the restore volume-add can always
			// name it. The slot is intentionally NOT released when the restore is accepted: the
			// edit log now references it.
			world_->queue_field_volume_upload(f.volume_slot, r.data);
			const ve::EditLog::AppendResult restore =
					world_->append_edit_locked(ve::make_volume_add(f.volume_slot, f.origin,
							f.voxel, f.dim));
			if (!restore.touched.empty()) restore_referenced_slot = true;
			const bool forced_restore_failure = debug_fail_next_restore_;
			debug_fail_next_restore_ = false;
			bool restored_all_carved = !forced_restore_failure && restore.rejected.empty() &&
					!restore.touched.empty();
			for (const ve::IVec3 &region : carved_regions)
				if (std::find(restore.touched.begin(), restore.touched.end(), region) ==
						restore.touched.end())
					restored_all_carved = false;
			if (restored_all_carved) {
				// The field has the rock back everywhere that was carved; the regions that
				// rejected the carve were never carved and already read solid. Despawn the
				// body and leave the component attached in the field.
				for (const ve::CellBox &box : f.boxes)
					for (int z = box.lo.z; z <= box.hi.z; z++)
						for (int y = box.lo.y; y <= box.hi.y; y++)
							for (int x = box.lo.x; x <= box.hi.x; x++)
								world_->occupancy().set_cell(
										{x, y, z}, ve::kCellSolid, world_->edit_seq());
				delete body;
				if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
				refused_++; DBG_LAND(carve_restored);
				return; // no hole: the component is back in the field, not a body
			}
			// The restore cannot cover every carved region. Keep the body alive so every
			// carved cell is occupied by a body; never despawn into a hole. Mark occupancy air
			// only for the regions the carve actually touched so the next connectivity pass
			// does not re-extract the same component.
			UtilityFunctions::printerr(
					"IslandManager: restore after carve rejection did not cover every carved "
					"region; keeping the already-spawned body in the hole");
			for (const ve::CellBox &box : f.boxes)
				for (int z = box.lo.z; z <= box.hi.z; z++)
					for (int y = box.lo.y; y <= box.hi.y; y++)
						for (int x = box.lo.x; x <= box.hi.x; x++) {
							const ve::IVec3 region =
									ve::WorldBounds::region_of_brick({x, y, z});
							if (std::find(carved_regions.begin(), carved_regions.end(), region) !=
									carved_regions.end())
								world_->occupancy().set_cell(
										{x, y, z}, ve::kCellAir, world_->edit_seq());
						}
			if (!restore_referenced_slot) world_->volumes().unpin(f.volume_slot);
			b = body;
		}
	}
	// The edit lock is released only after the carve either has a live body in the hole or
	// has put the rock back. No tool-thread edit can create a carved cell with neither.

	// Reuse a hole left by a despawn; append only when there is none.
	{
		size_t k = 0;
		while (k < bodies_.size() && bodies_[k] != nullptr) k++;
		if (k < bodies_.size()) bodies_[k] = b;
		else bodies_.push_back(b);
	}

	if (debris) debris_spawned_++;
	else islands_spawned_++;

	// 4. The raymarcher needs the bytes (spec §3's dense per-island texture). The upload
	//    carries BOTH slots: atlas slot for descriptor/mip entries, volume slot for the
	//    shared SDF/material/normal buffers (Task 6).
	if (atlas_slot >= 0) world_->queue_island_upload(atlas_slot, f.volume_slot, r.data);
}

void IslandManager::start_merges() {
	// submit_extracts() accepts only one batch at a time. A connectivity pass earlier in this
	// frame may already have submitted field extractions, or an earlier resample may still be
	// in flight; do not allocate an out-slot and push a Merging entry that can never receive a
	// result.
	if (world_->mesh_service()->extracts_busy()) return;
	std::vector<IslandExtractJob> jobs;
	for (size_t i = 0; i < bodies_.size(); i++) {
		IslandBody *b = bodies_[i];
		if (!b || !b->live() || b->asleep_seconds() < merge_sleep_s_) continue;
		if (std::any_of(merging_.begin(), merging_.end(),
					[&](const Merging &m) { return m.body_index == static_cast<int>(i); }))
			continue;
		if (merge_retry_blocked(static_cast<int>(i)))
			continue; // a rejected paste is still blocking this body
		const Transform3D xf = b->transform();
		// Ground contact is measured under the compound, not at the body origin. A wide body
		// resting on a crater rim reads as airborne when only the origin's xz is probed: the
		// centre probe lands in the dish BELOW the rim its boxes actually rest on (the island
		// sleeps at COM ~2.3 m over the centre probe and the merge never starts). Take the
		// compound's lowest box corner against the HIGHEST terrain hit under its XZ footprint.
		float com[3] = {0, 0, 0};
		float box_volume = 0.0f;
		ve::box_compound_mass(b->info().boxes.data(),
				static_cast<int>(b->info().boxes.size()), com, &box_volume);
		float bottom = 1e30f;
		float fp_lo_x = 1e30f, fp_lo_z = 1e30f, fp_hi_x = -1e30f, fp_hi_z = -1e30f;
		for (const ve::CellBox &box : b->info().boxes) {
			float lo[3], hi[3];
			box.world_aabb(lo, hi);
			for (int c8 = 0; c8 < 8; c8++) {
				const Vector3 local((c8 & 1) ? hi[0] - com[0] : lo[0] - com[0],
						(c8 & 2) ? hi[1] - com[1] : lo[1] - com[1],
						(c8 & 4) ? hi[2] - com[2] : lo[2] - com[2]);
				const Vector3 w = xf.xform(local);
				bottom = std::min(bottom, static_cast<float>(w.y));
				fp_lo_x = std::min(fp_lo_x, static_cast<float>(w.x));
				fp_hi_x = std::max(fp_hi_x, static_cast<float>(w.x));
				fp_lo_z = std::min(fp_lo_z, static_cast<float>(w.z));
				fp_hi_z = std::max(fp_hi_z, static_cast<float>(w.z));
			}
		}
		const float probes[5][2] = {{xf.origin.x, xf.origin.z},
				{fp_lo_x, fp_lo_z}, {fp_lo_x, fp_hi_z}, {fp_hi_x, fp_lo_z}, {fp_hi_x, fp_hi_z}};
		float best_ground = -1e30f;
		bool any_ground = false;
		for (const float(&p)[2] : probes) {
			const ve::RayHit g = world_->analytic_raycast_down(p);
			if (g.hit) {
				any_ground = true;
				best_ground = std::max(best_ground, g.pos[1]);
			}
		}
		if (!any_ground || bottom > best_ground + kMergeGroundClearanceM)
			continue; // still in the air: do not paste floating terrain
		const ve::VolumeData *src = world_->volumes().get(b->info().volume_slot);
		if (!src) continue;
		int out = world_->volumes().allocate();
		// At the 64-body cap every volume slot belongs to a live body, so there is no second
		// slot to resample into. Reuse the body's own birth slot instead: the worker already
		// receives a copy of `source`, and land_resample() keeps a second copy so a failed
		// store/pin or fully rejected paste can restore the live body's volume. land_resample()
		// preflights the exact paste regions under the edit mutex before writing the slot, so
		// a partially accepted paste can never corrupt the birth slot. This is a deliberate
		// improvement over the brief's allocate-a-second-slot approach (documented in the task
		// report).
		if (out < 0 && !world_->volumes().pinned(b->info().volume_slot))
			out = b->info().volume_slot;
		if (out < 0) {
			// Fail-soft (see the plan's Deliberate Deferrals): the body stays a body. It is
			// still collidable and still drawn; it just never becomes terrain.
			if (refused_++ == 0)
				UtilityFunctions::printerr(
						"IslandManager: volume pool full; sleeping islands stay as bodies");
			continue;
		}
		const bool reuses_birth_slot = out == b->info().volume_slot;

		IslandExtractJob job;
		job.kind = kResampleVolume;
		job.id = next_id_++;
		job.dim = b->info().dim;
		job.source = *src;
		// The birth placement, in the body's LOCAL frame: that is what the volume's samples
		// are indexed against, and it is exactly what IslandBody handed the raymarcher.
		job.source_op = ve::make_volume_add(b->info().volume_slot, b->local_lattice_origin(),
				b->info().voxel, b->info().dim);
		// ve::resample_volume takes the rotation ROW major, and Godot's Basis indexes rows.
		for (int a = 0; a < 3; a++)
			for (int k = 0; k < 3; k++) job.basis[a * 3 + k] = xf.basis[a][k];
		job.rest_origin[0] = xf.origin.x;
		job.rest_origin[1] = xf.origin.y;
		job.rest_origin[2] = xf.origin.z;
		job.out_slot = out;
		job.gen = gen_;
		Merging m;
		m.body_index = static_cast<int>(i);
		m.out_slot = out;
		m.submitted_transform = xf;
		if (reuses_birth_slot) m.source = *src;
		merging_.push_back(std::move(m));
		jobs.push_back(std::move(job));
		break; // one re-merge in flight at a time: the paste changes the field under the rest
	}
	if (jobs.empty()) return;
	if (!world_->mesh_service()->submit_extracts(std::move(jobs))) {
		// Defensive: even with the busy pre-check, a rejected submit must not leave a Merging
		// entry waiting for a result that will never arrive. Roll back the entry and free any
		// separately allocated out-slot. When the body's own birth slot was reused, no store or
		// pin happened yet, so popping the entry is sufficient to keep the live body intact.
		const Merging &m = merging_.back();
		const bool reuses_birth_slot = m.body_index >= 0 &&
				m.body_index < static_cast<int>(bodies_.size()) && bodies_[m.body_index] &&
				m.out_slot == bodies_[m.body_index]->info().volume_slot;
		if (!reuses_birth_slot) world_->release_volume_slot(m.out_slot);
		merging_.pop_back();
	}
}

void IslandManager::land_resample(const IslandExtractResult &r) {
	if (merging_.empty()) return;
	const Merging m = merging_.front();
	merging_.erase(merging_.begin());
	const bool invalid_body = m.body_index < 0 || m.body_index >= static_cast<int>(bodies_.size()) ||
			!bodies_[m.body_index] || !bodies_[m.body_index]->live();
	const bool reuses_birth_slot =
			!invalid_body && m.out_slot == bodies_[m.body_index]->info().volume_slot;
	if (r.failed || debug_fail_next_resample_ || invalid_body) {
		debug_fail_next_resample_ = false;
		// A failed resample never stored into the out-slot. When that slot is the live
		// body's birth slot it must stay allocated; only a separately allocated out-slot is
		// released.
		if (!reuses_birth_slot) world_->release_volume_slot(m.out_slot);
		if (!invalid_body) {
			// A failed resample is not a paste rejection, but it must still back off: without
			// a merge-retry entry the same body would be resubmitted every frame.
			note_merge_rejected(m.body_index, ve::EditLog::AppendResult{});
			refused_++;
		}
		return;
	}

	// The resample was submitted from a captured rest pose. If the body woke or moved while
	// the worker was busy, pasting the old pose and despawning the body from its current pose
	// would leave a ghost: the live body disappears from where it is, and terrain appears at
	// where it was. Keep the body alive and back off instead.
	if (bodies_[m.body_index]->asleep_seconds() <= 0.0f ||
			!same_rest_pose(bodies_[m.body_index]->transform(), m.submitted_transform)) {
		if (!reuses_birth_slot) world_->release_volume_slot(m.out_slot);
		note_merge_rejected(m.body_index, ve::EditLog::AppendResult{});
		refused_++;
		return; // stale rest pose: no paste, no despawn, the body stays a body
	}

	// Compute the exact region set the resample's volume-add will touch. The paste must be
	// fully accepted before we despawn, and it must be fully accepted before we overwrite a
	// reused birth slot. The resample result is already available here, so unlike a carve we
	// can check the actual op rather than a conservative AABB.
	std::vector<ve::IVec3> paste_regions;
	{
		ve::IVec3 rlo, rhi;
		ve::op_region_range(r.op, &rlo, &rhi);
		for (int z = rlo.z; z <= rhi.z; z++)
			for (int y = rlo.y; y <= rhi.y; y++)
				for (int x = rlo.x; x <= rhi.x; x++) {
					const ve::IVec3 region{x, y, z};
					if (std::find(paste_regions.begin(), paste_regions.end(), region) ==
							paste_regions.end())
						paste_regions.push_back(region);
				}
	}

	const Transform3D rest = bodies_[m.body_index]->transform();

	// Store, PIN, upload and append under ONE edit_mutex_ hold. The preflight below verifies
	// every region the paste will touch has room for the op; because append_edit_locked runs
	// under the same lock, no tool-thread edit can fill a region between the check and the
	// append. A paste that passes preflight is therefore guaranteed to be fully accepted --
	// which is what makes reusing the body's own birth slot safe. If we cannot guarantee
	// that, we leave the birth slot untouched and back off.
	{
		std::lock_guard<std::mutex> lock(world_->edit_mutex());
		ve::EditLog::AppendResult preflight;
		for (const ve::IVec3 &region : paste_regions)
			if (world_->edit_log()->op_count(region) >= ve::kMaxRegionOps)
				preflight.rejected.push_back(region);
		if (!preflight.rejected.empty()) {
			// No store or pin happened, so a reused birth slot still holds the body's
			// original volume. A separately allocated out-slot is unreferenced and can be
			// released.
			if (!reuses_birth_slot) world_->release_volume_slot(m.out_slot);
			note_merge_rejected(m.body_index, preflight);
			refused_++;
			return;
		}

		// Store, PIN and upload BEFORE the op reaches the log: once an op names a slot the
		// slot can never be reused, and the two GPU mirrors must already hold the bytes or a
		// brick regenerated this frame would read an empty slot. If either store or pin fails,
		// the out-slot is not referenced by any edit and can be released; when the out-slot is
		// the body's own birth slot, restore the original source bytes instead so the live body
		// keeps its volume.
		const bool stored = world_->volumes().store(m.out_slot, r.data);
		if (!stored || !world_->volumes().pin(m.out_slot)) {
			if (reuses_birth_slot) {
				if (stored) world_->volumes().store(m.out_slot, m.source);
			} else {
				world_->release_volume_slot(m.out_slot);
			}
			refused_++;
			return;
		}
		world_->queue_field_volume_upload(m.out_slot, r.data);

		// Spec §5 step 4: "stamped back as a CSG paste-op ... Rubble permanently accumulates".
		// A rejected paste means the field did NOT take the rock back. The body must remain a
		// body so the carved hole still has something in it. Never despawn unless the accepted
		// paste actually covers every region of the rest volume.
		const ve::EditLog::AppendResult paste = world_->append_edit_locked(r.op);
		const bool paste_covers = paste.rejected.empty() && !paste_regions.empty() &&
				!paste.touched.empty() &&
				std::all_of(paste_regions.begin(), paste_regions.end(),
						[&](const ve::IVec3 &region) {
							return std::find(paste.touched.begin(), paste.touched.end(), region) !=
									paste.touched.end();
						});
		if (!paste_covers) {
			// Defensive: preflight plus the same-lock append should make this unreachable. A
			// fully rejected paste (nothing touched) never referenced the slot, so discard the
			// uploads, unpin, and restore/release it. A partially accepted paste is the
			// corruption case this preflight exists to prevent: when the out-slot is the
			// body's own birth slot it is now pinned by accepted ops and cannot be restored,
			// so keep the body alive and permanently stop re-merging it rather than resample
			// from a world-aligned slot using the original birth lattice.
			if (paste.touched.empty()) {
				world_->discard_field_volume_upload(m.out_slot);
				world_->volumes().unpin(m.out_slot);
				if (reuses_birth_slot) {
					world_->volumes().store(m.out_slot, m.source);
				} else {
					world_->release_volume_slot(m.out_slot);
				}
			} else if (reuses_birth_slot) {
				block_merge_permanently(m.body_index);
			}
			note_merge_rejected(m.body_index, paste);
			refused_++;
			return;
		}

		last_merge_xz_[0] = rest.origin.x;
		last_merge_xz_[1] = rest.origin.z;
		despawn(m.body_index);
		islands_merged_++;
	}
}

void IslandManager::publish_descriptors() {
	std::vector<IslandSlotDesc> descs(kMaxIslands);
	for (IslandBody *b : bodies_) {
		if (!b || !b->live() || b->info().atlas_slot < 0) continue;
		const Transform3D xf = b->transform();
		IslandSlotDesc &d = descs[static_cast<size_t>(b->info().atlas_slot)];
		d.live = true;
		d.dim = b->info().dim;
		d.voxel = b->info().voxel;
		// Task 6: the raymarcher strides the shared authoritative volume buffers with THIS
		// slot, not the atlas slot.
		d.volume_slot = b->info().volume_slot;
		// COLUMN major: basis[a] is the world direction of local +a, which is what
		// Basis::get_column returns and what the shader's mat3(c0, c1, c2) expects.
		for (int a = 0; a < 3; a++) {
			const Vector3 c = xf.basis.get_column(a);
			d.basis[a * 3 + 0] = c.x;
			d.basis[a * 3 + 1] = c.y;
			d.basis[a * 3 + 2] = c.z;
		}
		d.origin[0] = xf.origin.x;
		d.origin[1] = xf.origin.y;
		d.origin[2] = xf.origin.z;
		for (int a = 0; a < 3; a++) d.lattice_origin[a] = b->local_lattice_origin()[a];
		d.recompute_world_aabb();
	}
	world_->publish_island_descriptors(descs);
}

int IslandManager::run_frame(float dt, const Vector3 &center) {
	if (!world_ || !world_->mesh_service()) return 0;
	const Clock::time_point t0 = Clock::now();
	int actions = 0;

	// 1. Bodies.
	for (IslandBody *b : bodies_)
		if (b && b->live()) {
			b->tick(dt);
			b->sync_render();
		}
	for (MergeRetry &r : merge_retries_)
		if (r.cooldown > 0) r.cooldown--;
	publish_descriptors();

	// 2. Results.
	std::vector<IslandExtractResult> results;
	world_->mesh_service()->collect_extracts(&results);
	for (const IslandExtractResult &r : results) {
		actions++;
		if (r.kind == kResampleVolume) land_resample(r);
		else land_extraction(r);
	}

	// 3. Connectivity, ONCE (spec §5). Held back while extractions are outstanding so a
	//    component cannot be labelled twice before its carve lands. The window queue is
	//    guarded because note_edit can be called from a tool thread under the edit mutex.
	bool run_window = false;
	PendingWindow w;
	{
		std::lock_guard<std::mutex> lock(windows_mutex_);
		if (!windows_.empty() && in_flight_.empty() &&
				!world_->mesh_service()->extracts_busy()) {
			w = windows_.front();
			if (w.retry_cooldown > 0) {
				// Backoff after a transient full-pool refusal; let the cooldown tick down
				// before relabelling.
				windows_.front().retry_cooldown--;
			} else if (live_body_count() + static_cast<int>(in_flight_.size()) >=
							max_dynamic_bodies_ ||
					world_->volumes().live_count() >= ve::kMaxVolumes) {
				// No body or volume capacity: keep the window queued instead of popping it
				// and losing the edit. Retrying is harmless here because the gate above is a
				// cheap counter check, not a connectivity relabel.
			} else if (window_is_fresh(w) || w.waited >= kMaxWindowWaitFrames) {
				windows_.pop_front();
				if (!window_is_fresh(w))
					UtilityFunctions::print_verbose(
							"IslandManager: occupancy readback is behind; running anyway");
				run_window = true;
			} else {
				windows_.front().waited++;
			}
		}
	}
	if (run_window) actions += run_connectivity(w);

	// 4. Re-merge.
	if (merging_.empty()) start_merges();

	// Spec §6's "small bubbles around active bodies": the collider streamer already accepts
	// N centres, and the bodies are the other N - 1.
	world_->set_physics_bubbles(bodies_);

	last_ms_ = std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
	(void)center;
	return actions;
}

int IslandManager::live_body_count() const {
	int n = 0;
	for (const IslandBody *b : bodies_)
		if (b && b->live()) n++;
	return n;
}

int IslandManager::free_atlas_slot() const {
	for (int i = 0; i < kMaxIslands; i++)
		if (!atlas_used_[static_cast<size_t>(i)]) return i;
	return -1;
}

void IslandManager::despawn(int index) {
	if (index < 0 || index >= static_cast<int>(bodies_.size()) || !bodies_[index]) return;
	IslandBody *b = bodies_[index];
	if (b->info().atlas_slot >= 0) {
		atlas_used_[static_cast<size_t>(b->info().atlas_slot)] = 0;
		// The descriptor is republished next frame with this slot dead, which is what stops
		// the raymarcher reading the bytes; they are left in place because nothing reads a
		// slot whose descriptor says dim 0.
	}
	// The BIRTH volume is normally never pinned -- no op in the edit log ever named it -- so
	// releasing it is safe. When re-merge reused the birth slot as the rested volume, the
	// paste op now pins it and it must stay for ever; release() would refuse it anyway, but
	// do not even try to free a slot the field owns.
	if (!world_->volumes().pinned(b->info().volume_slot))
		world_->release_volume_slot(b->info().volume_slot);
	delete b;
	bodies_[index] = nullptr; // a hole, not an erase: Merging::body_index must stay valid
	merge_retries_.erase(std::remove_if(merge_retries_.begin(), merge_retries_.end(),
					[&](const MergeRetry &r) { return r.body_index == index; }),
			merge_retries_.end());
}

#ifdef DEBUG_ENABLED
void IslandManager::debug_wake_body(int index) {
	if (index < 0 || index >= static_cast<int>(bodies_.size()) || !bodies_[index] ||
			!bodies_[index]->live())
		return;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return;
	ps->body_set_state(bodies_[index]->body(), PhysicsServer3D::BODY_STATE_SLEEPING, false);
}

Dictionary IslandManager::debug_body_info(int index) {
	Dictionary d;
	if (index < 0 || index >= static_cast<int>(bodies_.size()) || !bodies_[index] ||
			!bodies_[index]->live())
		return d;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return d;
	RID b = bodies_[index]->body();
	d["sleeping"] = static_cast<bool>(
			ps->body_get_state(b, PhysicsServer3D::BODY_STATE_SLEEPING));
	d["velocity"] = ps->body_get_state(b, PhysicsServer3D::BODY_STATE_LINEAR_VELOCITY);
	d["mass"] = static_cast<float>(ps->body_get_param(b, PhysicsServer3D::BODY_PARAM_MASS));
	d["gravity_scale"] = static_cast<float>(
			ps->body_get_param(b, PhysicsServer3D::BODY_PARAM_GRAVITY_SCALE));
	d["mode"] = static_cast<int>(ps->body_get_mode(b));
	d["layer"] = static_cast<int>(ps->body_get_collision_layer(b));
	d["shapes"] = bodies_[index]->shape_count();
	// A 0.5 m downward motion query against the body's own space: the direct answer to
	// "is there anything under it at all?"
	Ref<PhysicsTestMotionParameters3D> p;
	p.instantiate();
	p->set_from(bodies_[index]->transform());
	p->set_motion(Vector3(0, -0.5, 0));
	Ref<PhysicsTestMotionResult3D> r;
	r.instantiate();
	const bool hit = ps->body_test_motion(b, p, r);
	d["down_hit"] = hit;
	if (hit) {
		d["down_hit_point"] = r->get_collision_point();
		d["down_hit_normal"] = r->get_collision_normal();
		d["down_travel"] = r->get_travel();
		d["down_hit_rid"] = r->get_collider_rid();
	}
	// Also probe +x: the down query's hit point suggested a wall on the body's left.
	Ref<PhysicsTestMotionParameters3D> p2;
	p2.instantiate();
	p2->set_from(bodies_[index]->transform());
	p2->set_motion(Vector3(-0.5, 0, 0));
	Ref<PhysicsTestMotionResult3D> r2;
	r2.instantiate();
	const bool hit2 = ps->body_test_motion(b, p2, r2);
	d["left_hit"] = hit2;
	if (hit2) {
		d["left_hit_point"] = r2->get_collision_point();
		d["left_hit_normal"] = r2->get_collision_normal();
		d["left_hit_rid"] = r2->get_collider_rid();
	}
	return d;
}

void IslandManager::debug_offset_body(int index, const Vector3 &offset) {
	if (index < 0 || index >= static_cast<int>(bodies_.size()) || !bodies_[index] ||
			!bodies_[index]->live())
		return;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return;
	const Transform3D xf = bodies_[index]->transform();
	Transform3D moved = xf;
	moved.origin += offset;
	ps->body_set_state(bodies_[index]->body(), PhysicsServer3D::BODY_STATE_TRANSFORM, moved);
	ps->body_set_state(bodies_[index]->body(), PhysicsServer3D::BODY_STATE_SLEEPING, false);
}
#endif

Dictionary IslandManager::stats() {
	Dictionary d;
	int live_bodies = 0, live_islands = 0, live_debris = 0, sleeping_bodies = 0;
	int live_atlas_islands = 0, live_island_render_meshes = 0, live_debris_render_meshes = 0;
	float lowest = 1e30f;
	for (IslandBody *b : bodies_) {
		if (!b || !b->live()) continue;
		live_bodies++;
		if (b->info().debris) {
			live_debris++;
			if (b->has_render_mesh()) live_debris_render_meshes++;
		} else {
			live_islands++;
			if (b->info().atlas_slot >= 0) live_atlas_islands++;
			if (b->has_render_mesh()) live_island_render_meshes++;
		}
		lowest = std::min(lowest, static_cast<float>(b->transform().origin.y));
		if (b->asleep_seconds() > 0.0f) sleeping_bodies++;
	}
	d["live_bodies"] = live_bodies;
	d["live_islands"] = live_islands;
	d["live_debris"] = live_debris;
	d["live_atlas_islands"] = live_atlas_islands;
	d["live_island_render_meshes"] = live_island_render_meshes;
	d["live_debris_render_meshes"] = live_debris_render_meshes;
	d["lowest_body_y"] = live_bodies > 0 ? lowest : 0.0f;
	d["sleeping_bodies"] = sleeping_bodies;
	d["islands_spawned"] = islands_spawned_;
	d["debris_spawned"] = debris_spawned_;
	d["islands_merged"] = islands_merged_;
	d["connectivity_runs"] = connectivity_runs_;
	d["refused"] = refused_;
	// Why a component was left attached, split by WHERE the decision was taken. The named
	// reasons below are the ones connectivity can see before it submits an extraction;
	// "refused_landing" is the remainder, taken once the extraction came back (a full island
	// atlas, a stale volume, a preflight that found no op headroom, a failed spawn). The two
	// groups sum to "refused" by construction, so a stall can always name its own cause.
	const int named = refused_box_merge_ + refused_lattice_ + refused_op_cap_ +
			refused_body_cap_ + refused_pool_full_ + refused_unavailable_ + refused_empty_;
	d["refused_box_merge"] = refused_box_merge_;
	d["refused_lattice"] = refused_lattice_;
	d["refused_op_cap"] = refused_op_cap_;
	d["refused_body_cap"] = refused_body_cap_;
	d["refused_pool_full"] = refused_pool_full_;
	d["refused_unavailable"] = refused_unavailable_;
	d["refused_empty"] = refused_empty_;
	d["refused_landing"] = refused_ - named;
	d["crumbled"] = crumbled_;
	d["components_labelled"] = components_labelled_;
#ifdef DEBUG_ENABLED
	{
		PackedInt32Array carved;
		for (const ve::CellBox &box : debug_carved_boxes_) {
			carved.push_back(box.lo.x);
			carved.push_back(box.lo.y);
			carved.push_back(box.lo.z);
			carved.push_back(box.hi.x);
			carved.push_back(box.hi.y);
			carved.push_back(box.hi.z);
		}
		d["carved_boxes"] = carved;
		d["land_atlas_full"] = debug_land_.atlas_full;
		d["land_store_failed"] = debug_land_.store_failed;
		d["land_no_edit_log"] = debug_land_.no_edit_log;
		d["land_preflight"] = debug_land_.preflight;
		d["land_stale"] = debug_land_.stale;
		d["land_pin_failed"] = debug_land_.pin_failed;
		d["land_spawn_failed"] = debug_land_.spawn_failed;
		d["land_carve_nothing"] = debug_land_.carve_nothing;
		d["land_carve_restored"] = debug_land_.carve_restored;
	}
#endif

	{
		std::lock_guard<std::mutex> lock(windows_mutex_);
		d["pending_windows"] = static_cast<int>(windows_.size());
	}
	d["in_flight"] = static_cast<int>(in_flight_.size());
	d["merging"] = static_cast<int>(merging_.size());
	d["volume_live"] = world_ ? world_->volumes().live_count() : 0;
	int volume_pinned = 0;
	if (world_)
		for (int i = 0; i < ve::kMaxVolumes; i++)
			if (world_->volumes().pinned(i)) volume_pinned++;
	d["volume_pinned"] = volume_pinned;
	d["manager_ms"] = last_ms_;
	// Where the ground is under the last body to fall, so a test can say "the rubble is
	// standing on it" without knowing the terrain's shape. ve::raycast reads the same field
	// the paste went into, which is the point of asking it rather than the physics.
	float ground = 0.0f;
	if (world_) {
		const ve::RayHit h = world_->analytic_raycast_down(last_merge_xz_);
		if (h.hit) ground = h.pos[1];
	}
	d["ground_y"] = ground;
	d["last_merge_x"] = last_merge_xz_[0];
	d["last_merge_z"] = last_merge_xz_[1];
	return d;
}
