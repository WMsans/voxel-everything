#include "physics/island_manager.h"
#include "voxel_world.h"
#include "mesh/box_merge.h"
#include "render/mesh_service.h"
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>

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
	if (!jobs.empty()) world_->mesh_service()->submit_extracts(std::move(jobs));
	return submitted;
}

void IslandManager::land_extraction(const IslandExtractResult &r) {
	auto it = std::find_if(in_flight_.begin(), in_flight_.end(),
			[&r](const InFlight &f) { return f.id == r.id; });
	if (it == in_flight_.end()) return;
	const InFlight f = *it;
	in_flight_.erase(it);
	if (r.failed || r.data.solid_voxels == 0) {
		world_->volumes().release(f.volume_slot);
		return; // nothing there after all: the terrain keeps whatever the boxes covered
	}

	const float solid_m3 = static_cast<float>(r.data.solid_voxels) * f.voxel * f.voxel * f.voxel;
	const bool debris = solid_m3 < kDebrisVolumeM3;
	int atlas_slot = -1;
	if (!debris) {
		atlas_slot = free_atlas_slot();
		if (atlas_slot < 0) {
			// Spec §5's "<=32 island bodies (oldest sleepers merge early)". Nothing is
			// carved yet, so refusing costs only that this piece stays put for now.
			refused_++;
			world_->volumes().release(f.volume_slot);
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

	// 1. Carve (spec §5 step 1). The boxes tile the component exactly, so this removes the
	//    material that just became a body and nothing else. Ordered AFTER the extraction so
	//    the volume holds the rock rather than the hole.
	for (const ve::CellBox &box : f.boxes)
		world_->append_edit(ve::make_box_subtract(box.lo, box.hi));
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
	//    rule forbids leaving a hole with no body.
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

	IslandBody *b = new IslandBody();
	const Ref<World3D> w3 = world_->get_world_3d();
	if (!b->spawn(w3.is_valid() ? w3->get_space() : RID(),
				w3.is_valid() ? w3->get_scenario() : RID(), info, &r.data)) {
		delete b;
		if (atlas_slot >= 0) atlas_used_[static_cast<size_t>(atlas_slot)] = 0;
		// Restore the terrain. The slot is stored and becomes pinned by the volume add, so
		// it is intentionally NOT released here. After the store() check above, pin() cannot
		// fail unless an internal invariant is broken; if it ever does, do NOT release the
		// slot into the carved hole -- keep the piece attached instead.
		if (world_->volumes().pin(f.volume_slot)) {
			world_->queue_field_volume_upload(f.volume_slot, r.data);
			world_->append_edit(ve::make_volume_add(f.volume_slot, f.origin, f.voxel, f.dim));
			// The field has the rock back; make the occupancy grid agree so a later
			// connectivity/anchoring pass does not see a phantom air pocket.
			for (const ve::CellBox &box : f.boxes)
				for (int z = box.lo.z; z <= box.hi.z; z++)
					for (int y = box.lo.y; y <= box.hi.y; y++)
						for (int x = box.lo.x; x <= box.hi.x; x++)
							world_->occupancy().set_cell(
									{x, y, z}, ve::kCellSolid, world_->edit_seq());
		} else {
			// The original slot is not referenced by any edit (it was only stored for the
			// body), so it can be released and re-stored as a fresh pinned restore volume.
			// Releasing first guarantees the allocate below has a free slot even if the pool
			// was full. This branch should be unreachable: after store() succeeded above,
			// pin() cannot fail unless an internal invariant is broken.
			world_->volumes().release(f.volume_slot);
			const int restore_slot = world_->volumes().allocate();
			if (restore_slot < 0 || !world_->volumes().store(restore_slot, r.data) ||
					!world_->volumes().pin(restore_slot)) {
				UtilityFunctions::printerr(
						"IslandManager: FATAL: cannot restore a carved island after spawn failure; "
						"aborting rather than leaving a hole in the field");
				std::abort();
			}
			world_->queue_field_volume_upload(restore_slot, r.data);
			world_->append_edit(ve::make_volume_add(restore_slot, f.origin, f.voxel, f.dim));
			// The field has the rock back; make the occupancy grid agree so a later
			// connectivity/anchoring pass does not see a phantom air pocket.
			for (const ve::CellBox &box : f.boxes)
				for (int z = box.lo.z; z <= box.hi.z; z++)
					for (int y = box.lo.y; y <= box.hi.y; y++)
						for (int x = box.lo.x; x <= box.hi.x; x++)
							world_->occupancy().set_cell(
									{x, y, z}, ve::kCellSolid, world_->edit_seq());
		}
		refused_++;
		return; // no hole: the carve stands and either volume add put the rock back
	}
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
	std::vector<IslandExtractJob> jobs;
	for (size_t i = 0; i < bodies_.size(); i++) {
		IslandBody *b = bodies_[i];
		if (!b || !b->live() || b->asleep_seconds() < merge_sleep_s_) continue;
		if (std::any_of(merging_.begin(), merging_.end(),
					[&](const Merging &m) { return m.body_index == static_cast<int>(i); }))
			continue;
		const Transform3D xf = b->transform();
		const float xz[2] = {xf.origin.x, xf.origin.z};
		const ve::RayHit ground = world_->analytic_raycast_down(xz);
		if (!ground.hit || xf.origin.y > ground.pos[1] + kMergeGroundClearanceM)
			continue; // still in the air: do not paste floating terrain
		const ve::VolumeData *src = world_->volumes().get(b->info().volume_slot);
		if (!src) continue;
		const int out = world_->volumes().allocate();
		if (out < 0) {
			// Fail-soft (see the plan's Deliberate Deferrals): the body stays a body. It is
			// still collidable and still drawn; it just never becomes terrain.
			if (refused_++ == 0)
				UtilityFunctions::printerr(
						"IslandManager: volume pool full; sleeping islands stay as bodies");
			continue;
		}

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
		merging_.push_back(Merging{static_cast<int>(i), out});
		jobs.push_back(std::move(job));
		break; // one re-merge in flight at a time: the paste changes the field under the rest
	}
	if (!jobs.empty()) world_->mesh_service()->submit_extracts(std::move(jobs));
}

void IslandManager::land_resample(const IslandExtractResult &r) {
	if (merging_.empty()) return;
	const Merging m = merging_.front();
	merging_.erase(merging_.begin());
	if (r.failed || m.body_index < 0 || m.body_index >= static_cast<int>(bodies_.size()) ||
			!bodies_[m.body_index]) {
		world_->volumes().release(m.out_slot);
		return;
	}
	// Store, PIN and upload BEFORE the op reaches the log: once an op names a slot the
	// slot can never be reused, and the two GPU mirrors must already hold the bytes or a
	// brick regenerated this frame would read an empty slot.
	world_->volumes().store(m.out_slot, r.data);
	world_->volumes().pin(m.out_slot);
	world_->queue_field_volume_upload(m.out_slot, r.data);

	// Spec §5 step 4: "stamped back as a CSG paste-op ... Rubble permanently accumulates".
	const Transform3D rest = bodies_[m.body_index]->transform();
	last_merge_xz_[0] = rest.origin.x;
	last_merge_xz_[1] = rest.origin.z;
	world_->append_edit(r.op);
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
	// The BIRTH volume is never pinned -- no op in the edit log ever named it -- so releasing
	// it is safe. The RESTED volume the paste created is pinned and stays for ever.
	world_->volumes().release(b->info().volume_slot);
	delete b;
	bodies_[index] = nullptr; // a hole, not an erase: Merging::body_index must stay valid
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
