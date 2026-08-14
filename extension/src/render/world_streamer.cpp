#include "render/world_streamer.h"
#include "voxel_world.h" // godot::PendingEdit
#include <algorithm>

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
	if (!rd || !atlas_ || !residency_ || !edit_log_ || !region_pass_ || !brick_gen_) return 0;

	// Drain the edit queue. The lock is held for a swap, never across GPU work.
	std::vector<PendingEdit> edits;
	{
		std::lock_guard<std::mutex> lock(*edit_mutex_);
		edits.swap(*pending_);
	}

	const ve::ResidencyPlan plan = residency_->update(cx, cy, cz);
	frame_edits_ = static_cast<int>(edits.size());

	// --- buffer_update phase (must precede compute_list_begin: Godot errors otherwise) ---
	for (const auto &l : plan.loads) {
		const std::vector<ve::EditOp> &ops = edit_log_->ops(l.region);
		// Unconditional, even when empty: the slot may be a recycled eviction still holding
		// the previous tenant's op count, and op_count is the only field the mark pass reads.
		atlas_->upload_region_ops(rd, l.slot, ops.data(), static_cast<int>(ops.size()));
		atlas_->set_region_map_entry(rd, l.map_index, l.slot);
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
			const std::vector<ve::EditOp> &ops = edit_log_->ops(region);
			atlas_->upload_region_ops(rd, slot, ops.data(), static_cast<int>(ops.size()));
			const ve::IVec3 r0{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			const EditJob job{region, slot,
					{std::max(blo.x, r0.x), std::max(blo.y, r0.y), std::max(blo.z, r0.z)},
					{std::min(bhi.x, r0.x + 31), std::min(bhi.y, r0.y + 31),
							std::min(bhi.z, r0.z + 31)},
					static_cast<int>(ops.size())};
			if (job.lo.x <= job.hi.x && job.lo.y <= job.hi.y && job.lo.z <= job.hi.z)
				edit_jobs.push_back(job);
			last_edit_center_[0] = pe.op.pos[0];
			last_edit_center_[1] = pe.op.pos[1];
			last_edit_center_[2] = pe.op.pos[2];
			last_edit_radius_ = pe.op.radius;
			last_edit_type_ = static_cast<int>(pe.op.type);
			last_edit_material_ = static_cast<int>(pe.op.material);
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
	for (const auto &l : plan.loads) {
		const std::vector<ve::EditOp> &ops = edit_log_->ops(l.region);
		const ve::IVec3 lo{l.region.x * ve::kRegionBricks, l.region.y * ve::kRegionBricks,
				l.region.z * ve::kRegionBricks};
		const ve::IVec3 hi{lo.x + 31, lo.y + 31, lo.z + 31};
		region_pass_->mark(rd, list, l.region, l.slot, lo, hi,
				static_cast<int>(ops.size()), false);
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
	return static_cast<int>(plan.loads.size() + plan.evicts.size() + edit_jobs.size());
}
