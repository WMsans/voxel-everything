#include "physics/island_manager.h"
#include "voxel_world.h"
#include "mesh/box_merge.h"
#include "render/mesh_service.h"
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

using namespace godot;

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
// A sleeping body is only re-merged once it is actually resting on the terrain, not while
// it is still airborne. The body origin sits roughly half a body-height above the contact
// surface, so a few metres of clearance covers the largest component without letting a
// floating sleeper paste rubble into the sky (which would immediately be re-extracted).
constexpr float kMergeGroundClearanceM = 2.0f;

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
		if (world_) world_->volumes().release(b->info().volume_slot);
		delete b;
	}
	bodies_.clear();
	for (const InFlight &f : in_flight_)
		if (world_) world_->volumes().release(f.volume_slot);
	in_flight_.clear();
	for (const Merging &m : merging_)
		if (world_) world_->volumes().release(m.out_slot);
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

	// Merge into an overlapping window rather than queueing a second one: spec §5 wants ONE
	// connectivity run per frame however many blasts landed, and two windows over the same
	// rubble would label the same component twice.
	for (PendingWindow &e : windows_) {
		const bool overlap = e.lo.x <= w.hi.x && e.hi.x >= w.lo.x && e.lo.y <= w.hi.y &&
				e.hi.y >= w.lo.y && e.lo.z <= w.hi.z && e.hi.z >= w.lo.z;
		if (!overlap) continue;
		e.lo = {std::min(e.lo.x, w.lo.x), std::min(e.lo.y, w.lo.y), std::min(e.lo.z, w.lo.z)};
		e.hi = {std::max(e.hi.x, w.hi.x), std::max(e.hi.y, w.hi.y), std::max(e.hi.z, w.hi.z)};
		e.seq = std::max(e.seq, w.seq);
		if (w.impulse_scale > e.impulse_scale) {
			e.impulse_scale = w.impulse_scale;
			for (int a = 0; a < 3; a++) e.impulse_from[a] = w.impulse_from[a];
		}
		return;
	}
	windows_.push_back(w);
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
	connectivity_runs_++;
	ve::FloodWindow w = ve::FloodWindow::around(pw.lo, pw.hi, ve::kFloodWindowCells);
	ve::LinkCuts cuts;
	ve::FloodResult r;
	LogContactProbe probe;
	probe.gen = &gen_;
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

	if (!world_->mesh_service()->extraction_available()) {
		// Permanent for this physics lifetime: every field extraction would fail because the
		// worker has no IslandExtractPass. Do not allocate volume slots, submit jobs, or
		// re-queue a remainder that can only fail forever. The components stay attached in
		// the field, which is the same fail-soft outcome the worker would produce.
		refused_ += static_cast<int>(comps.size());
		return 0;
	}

	int submitted = 0;
	bool transient_refusal = false;
	std::vector<IslandExtractJob> jobs;
	for (const ve::IslandComponent &c : comps) {
		if (submitted >= kExtractsPerFrame) break;
		if (live_body_count() + static_cast<int>(in_flight_.size()) >= max_dynamic_bodies_) {
			refused_++;
			transient_refusal = true;
			break;
		}

		std::vector<ve::CellBox> boxes;
		if (!ve::greedy_box_merge(c.cells, ve::kMaxIslandBoxes, &boxes)) {
			// Fail-soft (spec §8): a shape too fragmented for 64 boxes stays attached. The
			// labeller's extent bound does not bound box COUNT, and a partial carve would
			// leave matter in two places at once.
			refused_++;
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
			continue;
		}
		const int slot = world_->volumes().allocate();
		if (slot < 0) {
			refused_++;
			transient_refusal = true;
			continue; // pool full: leave it attached
		}
		{
			std::lock_guard<std::mutex> lock(world_->edit_mutex());
			job.ops = world_->edit_log()->ops(ve::WorldBounds::region_of_brick(c.lo));
		}

		InFlight f;
		f.id = job.id;
		f.boxes = boxes;
		f.volume_slot = slot;
		f.voxel = job.voxel;
		f.dim = job.dim;
		f.window = pw;
		for (int a = 0; a < 3; a++) f.origin[a] = job.origin[a];
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
			world_->volumes().release(f.volume_slot);
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
		merge_retries_.push_back(MergeRetry{body_index, kMergeRetryCooldownFrames, paste.rejected});
		return;
	}
	it->cooldown = kMergeRetryCooldownFrames;
	for (const ve::IVec3 &region : paste.rejected)
		if (std::find(it->blocked_regions.begin(), it->blocked_regions.end(), region) ==
				it->blocked_regions.end())
			it->blocked_regions.push_back(region);
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
		if (it->cooldown > 0) return true;
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

void IslandManager::land_extraction(const IslandExtractResult &r) {
	auto it = std::find_if(in_flight_.begin(), in_flight_.end(),
			[&r](const InFlight &f) { return f.id == r.id; });
	if (it == in_flight_.end()) return;
	const InFlight f = *it;
	in_flight_.erase(it);
	if (r.failed || r.data.solid_voxels == 0) {
		world_->volumes().release(f.volume_slot);
		if (r.failed) note_extract_failure(f.window);
		else note_extract_success(f.window);
		return; // nothing there after all: the terrain keeps whatever the boxes covered
	}
	// A successful extraction is progress on this window; reset any prior failure streak so
	// only persistently failing batches accumulate toward dropping the remainder.
	note_extract_success(f.window);

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
			refused_++;
			world_->volumes().release(f.volume_slot);
			queue_retry_window(f.window);
			return;
		}
		atlas_used_[static_cast<size_t>(atlas_slot)] = 1;
		slot_high_water_ = std::max(slot_high_water_, atlas_slot + 1);
	}

	if (!world_->volumes().store(f.volume_slot, r.data)) {
		if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
		world_->volumes().release(f.volume_slot);
		refused_++;
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
			if (!world_->edit_log()->bounds().contains_region(r)) return;
			if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
		};
		for (const ve::CellBox &box : f.boxes) {
			ve::IVec3 rlo, rhi;
			ve::op_region_range(ve::make_box_subtract(box.lo, box.hi), &rlo, &rhi);
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
				ve::op_region_range(ve::make_box_subtract(box.lo, box.hi), &brlo, &brhi);
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
		if (!has_restore_headroom()) {
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			world_->volumes().release(f.volume_slot);
			refused_++;
			return; // preflight refused: no carve, no hole, the component stays attached
		}

		// Pin the birth volume before the first carve. The slot is already stored; pinning
		// here means every later restore path can reference it, and a pin failure happens
		// before any hole exists, so the stored slot can still be released.
		if (!world_->volumes().pin(f.volume_slot)) {
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			world_->volumes().release(f.volume_slot);
			refused_++;
			return; // no carve happened: the component stays attached
		}

		// 1. Carve (spec §5 step 1). The boxes tile the component exactly, so this removes the
		//    material that just became a body and nothing else. Ordered AFTER the extraction so
		//    the volume holds the rock rather than the hole.
		bool carve_rejected = false;
		std::vector<ve::IVec3> carved_regions;
		for (const ve::CellBox &box : f.boxes) {
			const ve::EditLog::AppendResult carve =
					world_->append_edit_locked(ve::make_box_subtract(box.lo, box.hi));
			for (const ve::IVec3 &region : carve.touched) carved_regions.push_back(region);
			if (!carve.rejected.empty()) {
				carve_rejected = true;
				break;
			}
		}

		// Spawn a body after a successful carve, restoring the field when the spawn cannot
		// complete. Returns the live body on success, or nullptr when the piece was restored /
		// left attached. If a restore was only partial, the edit log may already reference the
		// birth volume, so a later successful spawn must leave that slot pinned forever. If the
		// last-resort retry also fails, the no-hole invariant is violated and the process is
		// stopped rather than returning with carved cells uncovered.
		bool restore_referenced_slot = false;
		auto spawn_or_restore = [&]() -> IslandBody * {
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

			IslandBody *body = new IslandBody();
			const bool spawn_failed = debug_fail_next_spawn_ ||
					!body->spawn(w3.is_valid() ? w3->get_space() : RID(),
							w3.is_valid() ? w3->get_scenario() : RID(), info, &r.data);
			debug_fail_next_spawn_ = false;
			if (!spawn_failed) {
				if (atlas_slot >= 0) {
					atlas_used_[static_cast<size_t>(atlas_slot)] = 1;
					slot_high_water_ = std::max(slot_high_water_, atlas_slot + 1);
				}
				// A live body's birth volume is normally unpinned. If a carve-rejection restore
				// or the spawn-failure restore already appended a volume-add naming this slot,
				// it must stay pinned forever no matter what body later claims the slot.
				if (!restore_referenced_slot) world_->volumes().unpin(f.volume_slot);
				return body;
			}

			delete body;
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			// Nothing was carved (e.g. an out-of-bounds edge case), so there is no hole to
			// restore; the stored birth slot is unreferenced and can be released.
			if (carved_regions.empty()) {
				world_->volumes().unpin(f.volume_slot);
				world_->volumes().release(f.volume_slot);
				refused_++;
				return nullptr;
			}
			const auto restore_covers_carved = [&](const ve::EditLog::AppendResult &restore) {
				if (!restore.rejected.empty() || restore.touched.empty()) return false;
				for (const ve::IVec3 &region : carved_regions)
					if (std::find(restore.touched.begin(), restore.touched.end(), region) ==
							restore.touched.end())
						return false;
				return true;
			};
			// Restore the terrain. The slot was pinned before the first carve, so the volume-add
			// can name it and the slot is intentionally NOT released here: the edit log now
			// references it.
			world_->queue_field_volume_upload(f.volume_slot, r.data);
			const ve::EditLog::AppendResult restore =
					world_->append_edit_locked(ve::make_volume_add(f.volume_slot, f.origin,
							f.voxel, f.dim));
			if (!restore.touched.empty()) restore_referenced_slot = true;
			const bool forced_restore_failure = debug_fail_next_restore_;
			debug_fail_next_restore_ = false;
			if (!forced_restore_failure && restore_covers_carved(restore)) {
				// The field has the rock back; make the occupancy grid agree so a later
				// connectivity/anchoring pass does not see a phantom air pocket.
				for (const ve::CellBox &box : f.boxes)
					for (int z = box.lo.z; z <= box.hi.z; z++)
						for (int y = box.lo.y; y <= box.hi.y; y++)
							for (int x = box.lo.x; x <= box.hi.x; x++)
								world_->occupancy().set_cell(
										{x, y, z}, ve::kCellSolid, world_->edit_seq());
				refused_++;
				return nullptr;
			}

			// Last-resort: try spawning once more. The first failure may have been a one-shot
			// debug/transient failure. If the retry succeeds, the body fills the hole; if the
			// restore touched any region, the slot is already referenced and stays pinned.
			IslandBody *retry = new IslandBody();
			const bool retry_failed = debug_fail_next_spawn_ ||
					!retry->spawn(w3.is_valid() ? w3->get_space() : RID(),
							w3.is_valid() ? w3->get_scenario() : RID(), info, &r.data);
			debug_fail_next_spawn_ = false;
			if (!retry_failed) {
				if (atlas_slot >= 0) {
					atlas_used_[static_cast<size_t>(atlas_slot)] = 1;
					slot_high_water_ = std::max(slot_high_water_, atlas_slot + 1);
				}
				if (!restore_referenced_slot) world_->volumes().unpin(f.volume_slot);
				return retry;
			}
			delete retry;
			UtilityFunctions::printerr(
					"IslandManager: restore volume-add rejected or did not cover every carved "
					"region and a retry spawn also failed; a carved hole has neither a body nor "
					"the rock back");
			CRASH_NOW_MSG(
					"IslandManager: no-hole invariant violated after carve (restore incomplete "
					"and retry spawn failed); preflight + edit-mutex hold make this unreachable");
		};

		if (carve_rejected) {
			// With the preflight and the carve under the same lock this is unreachable under
			// op-cap pressure. It remains as a defensive fail-soft branch: if a carve was
			// rejected (possibly after some boxes were already removed), do NOT spawn a body
			// into a field that still contains the rock. Restore any accepted carve with a
			// pinned volume add and keep the component attached. Full regions that rejected
			// the carve were never carved, so a volume add rejected there is harmless; every
			// region that WAS carved must accept the restore.
			if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
			if (carved_regions.empty()) {
				world_->volumes().unpin(f.volume_slot);
				world_->volumes().release(f.volume_slot);
				refused_++;
				return; // nothing was carved: the component simply stays attached
			}
			// The slot was pinned before the first carve, so no pin can fail here; the restore
			// volume-add can always name it.
			world_->queue_field_volume_upload(f.volume_slot, r.data);
			const ve::EditLog::AppendResult restore =
					world_->append_edit_locked(ve::make_volume_add(f.volume_slot, f.origin,
							f.voxel, f.dim));
			if (!restore.touched.empty()) restore_referenced_slot = true;
			bool restored_all_carved = !restore.touched.empty();
			for (const ve::IVec3 &region : carved_regions)
				if (std::find(restore.touched.begin(), restore.touched.end(), region) ==
						restore.touched.end())
					restored_all_carved = false;
			if (restored_all_carved) {
				// The field has the rock back everywhere that was carved; the regions that
				// rejected the carve were never carved and already read solid.
				for (const ve::CellBox &box : f.boxes)
					for (int z = box.lo.z; z <= box.hi.z; z++)
						for (int y = box.lo.y; y <= box.hi.y; y++)
							for (int x = box.lo.x; x <= box.hi.x; x++)
								world_->occupancy().set_cell(
										{x, y, z}, ve::kCellSolid, world_->edit_seq());
				refused_++;
				return; // no hole: the component is back in the field, not a body
			}
			// Preflight + atomic lock made this unreachable under normal op-cap pressure. If
			// the restore still did not cover every carved region, fall through to the
			// body-preserving spawn path rather than aborting a release build.
			UtilityFunctions::printerr(
					"IslandManager: restore after carve rejection did not cover every carved "
					"region; falling back to spawning a body");
		}

		// 2. Tell the occupancy grid straight away. The GPU readback that would say the same
		//    thing is several frames out, and until it lands the next connectivity run would
		//    find this component all over again and carve it twice.
		for (const ve::CellBox &box : f.boxes)
			for (int z = box.lo.z; z <= box.hi.z; z++)
				for (int y = box.lo.y; y <= box.hi.y; y++)
					for (int x = box.lo.x; x <= box.hi.x; x++)
						world_->occupancy().set_cell({x, y, z}, ve::kCellAir, world_->edit_seq());

		// 3. Spawn (spec §5 step 3). Spawned AFTER the carve so the body is born into the hole,
		//    not overlapping the static terrain it just left. If spawn still fails, restore the
		//    rock as a pinned volume add: the carve already happened, and the global fail-soft
		//    rule forbids leaving a hole with no body. The edit lock is held across this so the
		//    restore cannot be rejected by a tool-thread edit that lands between carve and
		//    restore.
		b = spawn_or_restore();
		if (!b) return;
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

	// 4. The raymarcher needs the bytes (spec §3's dense per-island texture).
	if (atlas_slot >= 0) world_->queue_island_upload(atlas_slot, r.data);
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
		const float xz[2] = {xf.origin.x, xf.origin.z};
		const ve::RayHit ground = world_->analytic_raycast_down(xz);
		if (!ground.hit || xf.origin.y > ground.pos[1] + kMergeGroundClearanceM)
			continue; // still in the air: do not paste floating terrain
		const ve::VolumeData *src = world_->volumes().get(b->info().volume_slot);
		if (!src) continue;
		int out = world_->volumes().allocate();
		// At the 64-body cap every volume slot belongs to a live body, so there is no second
		// slot to resample into. Reuse the body's own birth slot instead: the worker already
		// receives a copy of `source`, and land_resample() keeps a second copy so a fully
		// rejected paste can restore the live body's volume. This is a deliberate improvement
		// over the brief's allocate-a-second-slot approach (documented in the task report).
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
		Merging m;
		m.body_index = static_cast<int>(i);
		m.out_slot = out;
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
		if (!reuses_birth_slot) world_->volumes().release(m.out_slot);
		merging_.pop_back();
	}
}

void IslandManager::land_resample(const IslandExtractResult &r) {
	if (merging_.empty()) return;
	const Merging m = merging_.front();
	merging_.erase(merging_.begin());
	const bool invalid_body = m.body_index < 0 || m.body_index >= static_cast<int>(bodies_.size()) ||
			!bodies_[m.body_index];
	const bool reuses_birth_slot =
			!invalid_body && m.out_slot == bodies_[m.body_index]->info().volume_slot;
	if (r.failed || debug_fail_next_resample_ || invalid_body) {
		debug_fail_next_resample_ = false;
		// A failed resample never stored into the out-slot. When that slot is the live
		// body's birth slot it must stay allocated; only a separately allocated out-slot is
		// released.
		if (!reuses_birth_slot) world_->volumes().release(m.out_slot);
		if (!invalid_body) {
			// A failed resample is not a paste rejection, but it must still back off: without
			// a merge-retry entry the same body would be resubmitted every frame.
			note_merge_rejected(m.body_index, ve::EditLog::AppendResult{});
			refused_++;
		}
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
			world_->volumes().release(m.out_slot);
		}
		refused_++;
		return;
	}
	world_->queue_field_volume_upload(m.out_slot, r.data);

	// Spec §5 step 4: "stamped back as a CSG paste-op ... Rubble permanently accumulates".
	// A rejected paste means the field did NOT take the rock back. The body must remain a
	// body so the carved hole still has something in it; the pinned out-slot is left
	// reserved (it may be referenced by regions that did accept the paste). If NO region
	// accepted the paste, the pin was never referenced by an op, so discard the uploads we
	// queued for it, unpin, and release it (or restore the live body's birth volume when the
	// out-slot was that same slot); otherwise it would leak forever because release() refuses
	// pinned slots.
	const auto release_rejected_out_slot = [&]() {
		world_->discard_field_volume_upload(m.out_slot);
		world_->volumes().unpin(m.out_slot);
		if (reuses_birth_slot) {
			world_->volumes().store(m.out_slot, m.source);
		} else {
			world_->volumes().release(m.out_slot);
		}
	};
	const Transform3D rest = bodies_[m.body_index]->transform();
	const ve::EditLog::AppendResult paste = world_->append_edit(r.op);
	if (!paste.rejected.empty()) {
		if (paste.touched.empty()) release_rejected_out_slot();
		note_merge_rejected(m.body_index, paste);
		refused_++;
		return;
	}
	if (paste.touched.empty()) {
		// Defensive: an out-of-bounds/edge-case paste added no terrain anywhere. Keep the
		// body alive and release the unreferenced pinned out-slot; never despawn into a hole.
		release_rejected_out_slot();
		note_merge_rejected(m.body_index, paste);
		refused_++;
		return;
	}
	last_merge_xz_[0] = rest.origin.x;
	last_merge_xz_[1] = rest.origin.z;
	despawn(m.body_index);
	islands_merged_++;
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
		world_->volumes().release(b->info().volume_slot);
	delete b;
	bodies_[index] = nullptr; // a hole, not an erase: Merging::body_index must stay valid
	merge_retries_.erase(std::remove_if(merge_retries_.begin(), merge_retries_.end(),
					[&](const MergeRetry &r) { return r.body_index == index; }),
			merge_retries_.end());
}

Dictionary IslandManager::stats() {
	Dictionary d;
	int live_bodies = 0, live_islands = 0, live_debris = 0;
	float lowest = 1e30f;
	for (IslandBody *b : bodies_) {
		if (!b || !b->live()) continue;
		live_bodies++;
		if (b->info().debris) live_debris++;
		else live_islands++;
		lowest = std::min(lowest, static_cast<float>(b->transform().origin.y));
	}
	d["live_bodies"] = live_bodies;
	d["live_islands"] = live_islands;
	d["live_debris"] = live_debris;
	d["lowest_body_y"] = live_bodies > 0 ? lowest : 0.0f;
	d["islands_spawned"] = islands_spawned_;
	d["debris_spawned"] = debris_spawned_;
	d["islands_merged"] = islands_merged_;
	d["connectivity_runs"] = connectivity_runs_;
	d["refused"] = refused_;
	{
		std::lock_guard<std::mutex> lock(windows_mutex_);
		d["pending_windows"] = static_cast<int>(windows_.size());
	}
	d["in_flight"] = static_cast<int>(in_flight_.size());
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
	return d;
}
