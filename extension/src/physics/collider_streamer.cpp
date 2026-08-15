#include "physics/collider_streamer.h"
#include "mesh/mesh_chunk.h"
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

namespace {

// The residency's view of the world field. NOTE the qualification on ve::chunk_has_surface:
// unqualified, the name would resolve to this override and recurse for ever.
struct LogProbe : ve::ChunkProbe {
	const ve::Generator *gen = nullptr;
	ve::EditLog *log = nullptr;
	std::mutex *mu = nullptr;

	bool chunk_has_surface(ve::IVec3 c) const override {
		std::lock_guard<std::mutex> lock(*mu);
		const std::vector<ve::EditOp> &ops = log->ops(ve::region_of_chunk(c));
		return ve::chunk_has_surface(*gen, ops.data(), static_cast<int>(ops.size()), c);
	}
};

} // namespace

ColliderStreamer::~ColliderStreamer() {
	teardown();
}

void ColliderStreamer::initialize(ve::ChunkResidency *chunks, ve::EditLog *edit_log,
		std::mutex *edit_mutex, MeshPass *mesh, int max_slots) {
	teardown();
	chunks_ = chunks;
	edit_log_ = edit_log;
	edit_mutex_ = edit_mutex;
	mesh_ = mesh;
	bodies_.assign(static_cast<size_t>(std::max(0, max_slots)), RID());
	shapes_.assign(static_cast<size_t>(std::max(0, max_slots)), RID());
	in_space_.assign(static_cast<size_t>(std::max(0, max_slots)), 0);
}

void ColliderStreamer::teardown() {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (ps) {
		for (size_t i = 0; i < bodies_.size(); i++) {
			if (bodies_[i].is_valid()) {
				ps->body_set_space(bodies_[i], RID());
				ps->free_rid(bodies_[i]);
			}
			if (shapes_[i].is_valid()) ps->free_rid(shapes_[i]);
		}
	}
	bodies_.clear();
	shapes_.clear();
	in_space_.clear();
	inbox_.clear();
	active_bodies_ = 0;
	failures_ = 0;
	overflow_warnings_ = 0;
	last_build_ms_ = 0.0f;
	chunks_ = nullptr;
	edit_log_ = nullptr;
	edit_mutex_ = nullptr;
	mesh_ = nullptr;
}

void ColliderStreamer::set_space(RID space) {
	if (space == space_) return;
	space_ = space;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return;
	// The space arrives once the node is in the tree, which can be after the first bodies
	// exist; re-home whatever is already live.
	for (size_t i = 0; i < bodies_.size(); i++)
		if (in_space_[i] && bodies_[i].is_valid()) ps->body_set_space(bodies_[i], space_);
}

float ColliderStreamer::last_collect_ms() const {
	return mesh_ ? mesh_->last_collect_ms() : 0.0f;
}

RID ColliderStreamer::body_of_slot(int slot) const {
	return slot >= 0 && slot < static_cast<int>(bodies_.size()) ? bodies_[slot] : RID();
}

ColliderStreamer::BuildOutcome ColliderStreamer::build_shape(int slot, const MeshResult &r) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || slot < 0 || slot >= static_cast<int>(bodies_.size())) return kFailed;
	const int verts = static_cast<int>(r.positions.size() / 3);

	// ConcavePolygonShape3D takes a de-indexed triangle soup; Godot's own resource sends
	// exactly this dictionary (scene/resources/3d/concave_polygon_shape_3d.cpp).
	PackedVector3Array faces;
	faces.resize(static_cast<int64_t>(r.indices.size()));
	Vector3 *fw = faces.ptrw();
	int n = 0;
	for (size_t t = 0; t + 2 < r.indices.size(); t += 3) {
		Vector3 v[3];
		bool ok = true;
		for (int k = 0; k < 3; k++) {
			const uint32_t vi = r.indices[t + k];
			if (static_cast<int>(vi) >= verts) { ok = false; break; }
			v[k] = Vector3(r.positions[vi * 3], r.positions[vi * 3 + 1], r.positions[vi * 3 + 2]);
		}
		if (!ok) continue;
		// Two dual vertices can coincide to float precision on a flat run of cells. Jolt
		// warns once per degenerate triangle it is handed, which would drown the log; drop
		// them here, where it costs one cross product.
		if ((v[1] - v[0]).cross(v[2] - v[0]).length_squared() < 1e-12f) continue;
		// The mesher's right-hand-rule winding (cross product points at air, verified by
		// debug_mesh_diff) is the opposite of Jolt's front-face convention for one-sided
		// concave shapes, so swap the last two vertices here. Without the swap a ray from
		// above misses and a character would fall through the terrain.
		fw[n++] = v[0];
		fw[n++] = v[2];
		fw[n++] = v[1];
	}
	faces.resize(n);
	if (n < 3) return kEmpty;

	Dictionary data;
	data["faces"] = faces;
	// Left false deliberately: the mesher's winding always faces the air, so one-sided
	// collision is correct everywhere, including inside a carved cave. Jolt also does not
	// implement the two-sided case.
	data["backface_collision"] = false;

	if (!shapes_[slot].is_valid()) shapes_[slot] = ps->concave_polygon_shape_create();
	if (!shapes_[slot].is_valid()) return kFailed;
	ps->shape_set_data(shapes_[slot], data);

	if (!bodies_[slot].is_valid()) {
		bodies_[slot] = ps->body_create();
		if (!bodies_[slot].is_valid()) return kFailed;
		ps->body_set_mode(bodies_[slot], PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_add_shape(bodies_[slot], shapes_[slot]);
		ps->body_set_collision_layer(bodies_[slot], 1);
		ps->body_set_collision_mask(bodies_[slot], 1);
		// Explicit: both backends default a server-created body to ray-pickable, and the
		// tests' intersect_ray depends on it.
		ps->body_set_ray_pickable(bodies_[slot], true);
		// Mesh positions are already world space, so the body never moves.
		ps->body_set_state(bodies_[slot], PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D());
	}
	ps->body_set_shape_disabled(bodies_[slot], 0, false);
	if (!in_space_[slot]) {
		ps->body_set_space(bodies_[slot], space_);
		in_space_[slot] = 1;
		active_bodies_++;
	}
	return kBuilt;
}

void ColliderStreamer::release_slot(int slot) {
	if (slot < 0 || slot >= static_cast<int>(bodies_.size())) return;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !in_space_[slot]) return;
	// The body and its shape RID are kept for reuse: walking in and out of the radius would
	// otherwise churn server allocations every few seconds, and out of the space they cost
	// Jolt nothing.
	ps->body_set_space(bodies_[slot], RID());
	in_space_[slot] = 0;
	active_bodies_--;
}

void ColliderStreamer::apply_result(const MeshResult &r) {
	const int slot = chunks_->slot_of(r.chunk);
	if (slot < 0) {
		// Evicted/displaced while the mesh was in flight; there is no slot to attach it to.
		// Still clear the outstanding-build marker, or the chunk can never be re-streamed.
		chunks_->note_discarded(r.chunk);
		return;
	}
	if (r.failed) {
		failures_++;
		UtilityFunctions::printerr("ColliderStreamer: readback failed for chunk (",
				r.chunk.x, ", ", r.chunk.y, ", ", r.chunk.z, ")");
		chunks_->note_failed(r.chunk);
		return;
	}
	if (r.overflow && overflow_warnings_ < 8) {
		overflow_warnings_++;
		UtilityFunctions::push_warning("ColliderStreamer: chunk (", r.chunk.x, ", ", r.chunk.y,
				", ", r.chunk.z, ") hit a mesher cap; collider built from what fit");
	}
	const uint64_t t0 = Time::get_singleton()->get_ticks_usec();
	const BuildOutcome outcome = build_shape(slot, r);
	last_build_ms_ =
			static_cast<float>(Time::get_singleton()->get_ticks_usec() - t0) / 1000.0f;
	switch (outcome) {
		case kBuilt:
			chunks_->note_built(r.chunk);
			break;
		case kEmpty:
			if (r.overflow) {
				// An overflowed mesh is missing pieces; "fewer than 3 triangles survived" is
				// not proof the chunk is empty. Caching empty would hide the real surface, so
				// fail and retry instead.
				failures_++;
				UtilityFunctions::printerr("ColliderStreamer: overflowed chunk (", r.chunk.x,
						", ", r.chunk.y, ", ", r.chunk.z,
						") left fewer than 3 triangles; treating as failed build");
				chunks_->note_failed(r.chunk);
			} else {
				// The probe is conservative, so a chunk it passed can hold no triangles at all.
				const int freed = chunks_->note_empty(r.chunk);
				release_slot(freed);
			}
			break;
		case kFailed:
			// Spec §6's failure policy: log, keep the previous collider, retry next frame.
			failures_++;
			UtilityFunctions::printerr("ColliderStreamer: shape build failed for chunk (",
					r.chunk.x, ", ", r.chunk.y, ", ", r.chunk.z, ")");
			chunks_->note_failed(r.chunk);
			break;
	}
}

int ColliderStreamer::run_frame(float cx, float cy, float cz) {
	if (!chunks_ || !mesh_ || !mesh_->is_valid()) return 0;
	int actions = 0;

	// 1. Land whatever the GPU finished. The sync inside collect() is for a batch submitted
	//    on an earlier frame, so it does not wait on the GPU.
	{
		std::vector<MeshResult> collected;
		if (mesh_->in_flight()) mesh_->collect(&collected);
		for (MeshResult &r : collected) inbox_.push_back(std::move(r));
	}

	// 2. Turn results into shapes, throttled: shape_set_data builds Jolt's BVH on this
	//    thread, and a 15k-triangle chunk is a millisecond or two of it.
	builds_last_frame_ = 0;
	while (!inbox_.empty() && builds_last_frame_ < max_builds_per_frame_) {
		MeshResult r = std::move(inbox_.front());
		inbox_.pop_front();
		apply_result(r);
		builds_last_frame_++;
		actions++;
	}

	// 3. Plan. No new work while a batch is in flight or results are still queued — the
	//    mesher holds one batch at a time, and a chunk planned now would only be dropped.
	const float center[3] = {cx, cy, cz};
	LogProbe probe;
	probe.gen = &gen_;
	probe.log = edit_log_;
	probe.mu = edit_mutex_;
	const int build_cap = (mesh_->in_flight() || !inbox_.empty()) ? 0 : -1;
	const ve::ChunkPlan plan = chunks_->update(center, nullptr, 1, probe, build_cap);
	for (const auto &e : plan.releases) {
		release_slot(e.slot);
		actions++;
	}

	// 4. Mesh. Each chunk lies inside exactly one region, so one op list reconstructs it.
	if (!plan.builds.empty()) {
		std::vector<std::vector<ve::EditOp>> ops;
		ops.reserve(plan.builds.size());
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			for (const auto &e : plan.builds)
				ops.push_back(edit_log_->ops(ve::region_of_chunk(e.chunk)));
		}
		std::vector<MeshJob> jobs;
		jobs.reserve(plan.builds.size());
		// Built only after every copy exists: a push_back that reallocated `ops` would
		// leave an earlier job pointing at freed memory.
		for (size_t i = 0; i < plan.builds.size(); i++)
			jobs.push_back({plan.builds[i].chunk, ops[i].data(), static_cast<int>(ops[i].size())});
		if (mesh_->submit(jobs.data(), static_cast<int>(jobs.size()))) {
			actions += static_cast<int>(jobs.size());
		} else {
			for (const auto &e : plan.builds) chunks_->note_failed(e.chunk);
		}
	}
	return actions;
}
