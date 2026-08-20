#include "physics/collider_streamer.h"
#include "mesh/mesh_chunk.h"
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>

using namespace godot;

namespace {

using Clock = std::chrono::steady_clock;
float ms_since(Clock::time_point t0) {
	return std::chrono::duration<float, std::milli>(Clock::now() - t0).count();
}

// What handing Jolt one triangle of a concave shape costs, measured on the shipping backend
// across 1k-64k triangle soups. This admits the NEXT octant build against the existing budget;
// a first build always runs so a large octant cannot wedge the queue forever.
constexpr float kShapeBuildMsPerTriangle = 0.00075f;

int sub_index(int slot, int octant) {
	return slot * ve::kColliderOctants + octant;
}

// The residency's view of the world field.
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
		std::mutex *edit_mutex, MeshService *mesh, int max_slots) {
	teardown();
	chunks_ = chunks;
	edit_log_ = edit_log;
	edit_mutex_ = edit_mutex;
	mesh_ = mesh;
	const size_t slots = static_cast<size_t>(std::max(0, max_slots));
	const size_t bodies = slots * ve::kColliderOctants;
	bodies_.assign(bodies, RID());
	shapes_.assign(bodies, RID());
	in_space_.assign(bodies, 0);
	build_counts_.assign(slots, 0);
	last_submit_ops_.assign(slots, -1);
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
		for (PendingBuild &pending : pending_) discard_pending(pending);
	}
	bodies_.clear();
	shapes_.clear();
	in_space_.clear();
	inbox_.clear();
	pending_.clear();
	active_bodies_ = 0;
	failures_ = 0;
	overflow_warnings_ = 0;
	max_build_tris_ = 0;
	max_chunk_tris_ = 0;
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
	// exist; re-home every live octant body.
	for (size_t i = 0; i < bodies_.size(); i++)
		if (in_space_[i] && bodies_[i].is_valid()) ps->body_set_space(bodies_[i], space_);
}

float ColliderStreamer::last_collect_ms() const {
	return mesh_ ? mesh_->last_collect_ms() : 0.0f;
}

int ColliderStreamer::bodies_in_space() const {
	int count = 0;
	for (char in_space : in_space_)
		if (in_space) count++;
	return count;
}

RID ColliderStreamer::body_of_slot(int slot) const {
	if (slot < 0 || slot >= static_cast<int>(build_counts_.size())) return RID();
	// Octant zero is allowed to be empty. Diagnostics must identify the chunk through any
	// populated octant, otherwise a valid multi-body collider looks absent at random.
	for (int octant = 0; octant < ve::kColliderOctants; octant++) {
		const RID body = bodies_[static_cast<size_t>(sub_index(slot, octant))];
		if (body.is_valid()) return body;
	}
	return RID();
}

int ColliderStreamer::build_count_of_chunk(ve::IVec3 c) const {
	if (!chunks_) return -1;
	const int slot = chunks_->slot_of(c);
	return slot >= 0 && slot < static_cast<int>(build_counts_.size())
			? build_counts_[static_cast<size_t>(slot)]
			: -1;
}

int ColliderStreamer::chunk_state(ve::IVec3 c) const {
	if (!chunks_) return -1;
	return chunks_->slot_state_of(c);
}

bool ColliderStreamer::chunk_in_flight(ve::IVec3 c) const {
	if (!chunks_) return false;
	return chunks_->build_in_flight(c);
}

int ColliderStreamer::last_submit_op_count(ve::IVec3 c) const {
	if (!chunks_) return -1;
	const int slot = chunks_->slot_of(c);
	return slot >= 0 && slot < static_cast<int>(last_submit_ops_.size())
			? last_submit_ops_[static_cast<size_t>(slot)]
			: -1;
}

void ColliderStreamer::free_slot_resources(int slot) {
	if (slot < 0 || slot >= static_cast<int>(build_counts_.size())) return;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return;
	for (int octant = 0; octant < ve::kColliderOctants; octant++) {
		const int index = sub_index(slot, octant);
		if (bodies_[index].is_valid()) {
			ps->body_set_space(bodies_[index], RID());
			ps->free_rid(bodies_[index]);
			bodies_[index] = RID();
		}
		if (shapes_[index].is_valid()) {
			ps->free_rid(shapes_[index]);
			shapes_[index] = RID();
		}
		in_space_[index] = 0;
	}
}

void ColliderStreamer::discard_pending(PendingBuild &pending) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps) return;
	for (RID &shape : pending.staged_shapes) {
		if (shape.is_valid()) ps->free_rid(shape);
		shape = RID();
	}
}

ColliderStreamer::BuildOutcome ColliderStreamer::build_octant(PendingBuild &pending, int octant) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || octant < 0 || octant >= ve::kColliderOctants) return kFailed;
	const std::vector<uint32_t> &bin = pending.bins[static_cast<size_t>(octant)];
	if (bin.empty()) return kEmpty;

	const Clock::time_point t_faces = Clock::now();
	const int verts = static_cast<int>(pending.result.positions.size() / 3);
	PackedVector3Array faces;
	faces.resize(static_cast<int64_t>(bin.size()));
	Vector3 *fw = faces.ptrw();
	int n = 0;
	for (size_t t = 0; t + 2 < bin.size(); t += 3) {
		Vector3 v[3];
		bool ok = true;
		for (int k = 0; k < 3; k++) {
			const uint32_t vi = bin[t + k];
			if (vi >= static_cast<uint32_t>(verts)) {
				ok = false;
				break;
			}
			v[k] = Vector3(pending.result.positions[vi * 3],
					pending.result.positions[vi * 3 + 1], pending.result.positions[vi * 3 + 2]);
		}
		if (!ok) continue;
		// Two dual vertices can coincide to float precision on a flat run of cells. Drop the
		// degenerate triangle before Jolt sees it, as the old one-body path did.
		if ((v[1] - v[0]).cross(v[2] - v[0]).length_squared() < 1e-12f) continue;
		// The mesher's right-hand-rule winding faces air; Jolt's one-sided concave convention
		// is opposite, so preserve the established swap exactly in every octant.
		fw[n++] = v[0];
		fw[n++] = v[2];
		fw[n++] = v[1];
	}
	faces.resize(n);
	last_faces_ms_ += ms_since(t_faces);
	last_tris_ += n / 3;
	if (n < 3) return kEmpty;
	// This is the largest single shape_set_data payload, not the frame's sum of octants.
	max_build_tris_ = std::max(max_build_tris_, n / 3);

	Dictionary data;
	data["faces"] = faces;
	data["backface_collision"] = false;
	const RID shape = ps->concave_polygon_shape_create();
	if (!shape.is_valid()) return kFailed;
	const Clock::time_point t_set = Clock::now();
	ps->shape_set_data(shape, data);
	last_setdata_ms_ = std::max(last_setdata_ms_, ms_since(t_set));
	pending.staged_shapes[static_cast<size_t>(octant)] = shape;
	pending.geometry_octants++;
	return kBuilt;
}

bool ColliderStreamer::commit_pending(PendingBuild &pending) {
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	if (!ps || !chunks_) return false;
	const int slot = chunks_->slot_of(pending.result.chunk);
	if (slot != pending.slot || chunks_->chunk_of_slot(slot) != pending.result.chunk ||
			chunks_->slot_state_of(pending.result.chunk) != ve::ChunkResidency::kBuilding)
		return false;

	if (pending.geometry_octants == 0) {
		// An overflowed result is incomplete evidence, including when every surviving triangle
		// was degenerate. Never cache it as empty: retain the previous bodies and put the chunk
		// back in the retry queue.
		if (pending.result.overflow) {
			failures_++;
			UtilityFunctions::printerr("ColliderStreamer: overflowed chunk (", pending.result.chunk.x,
					", ", pending.result.chunk.y, ", ", pending.result.chunk.z,
					") produced no valid collider geometry; keeping the previous collider and retrying");
			chunks_->note_failed(pending.result.chunk);
			return true;
		}
		const int freed = chunks_->note_empty(pending.result.chunk);
		release_slot(freed);
		return true;
	}

	std::array<RID, ve::kColliderOctants> new_bodies;
	const Clock::time_point t_body = Clock::now();
	for (int octant = 0; octant < ve::kColliderOctants; octant++) {
		const RID shape = pending.staged_shapes[static_cast<size_t>(octant)];
		if (!shape.is_valid()) continue;
		RID body = ps->body_create();
		if (!body.is_valid()) {
			for (RID &created : new_bodies) {
				if (created.is_valid()) ps->free_rid(created);
				created = RID();
			}
			return false;
		}
		ps->body_set_mode(body, PhysicsServer3D::BODY_MODE_STATIC);
		ps->body_add_shape(body, shape);
		ps->body_set_collision_layer(body, 1);
		ps->body_set_collision_mask(body, 1);
		ps->body_set_ray_pickable(body, true);
		ps->body_set_state(body, PhysicsServer3D::BODY_STATE_TRANSFORM, Transform3D());
		new_bodies[static_cast<size_t>(octant)] = body;
	}
	last_body_ms_ += ms_since(t_body);

	bool was_active = false;
	for (int octant = 0; octant < ve::kColliderOctants; octant++)
		if (in_space_[sub_index(slot, octant)]) was_active = true;
	free_slot_resources(slot);
	for (int octant = 0; octant < ve::kColliderOctants; octant++) {
		const int index = sub_index(slot, octant);
		bodies_[index] = new_bodies[static_cast<size_t>(octant)];
		shapes_[index] = pending.staged_shapes[static_cast<size_t>(octant)];
		pending.staged_shapes[static_cast<size_t>(octant)] = RID();
		if (bodies_[index].is_valid()) {
			ps->body_set_space(bodies_[index], space_);
			in_space_[index] = 1;
		}
	}
	if (!was_active) active_bodies_++;
	chunks_->note_built(pending.result.chunk);
	return true;
}

void ColliderStreamer::release_slot(int slot) {
	if (slot < 0 || slot >= static_cast<int>(build_counts_.size())) return;
	bool was_active = false;
	PhysicsServer3D *ps = PhysicsServer3D::get_singleton();
	for (int octant = 0; octant < ve::kColliderOctants; octant++) {
		const int index = sub_index(slot, octant);
		if (in_space_[index]) {
			was_active = true;
			if (ps && bodies_[index].is_valid()) ps->body_set_space(bodies_[index], RID());
			in_space_[index] = 0;
		}
	}
	if (was_active) active_bodies_--;
}

void ColliderStreamer::enqueue_result(MeshResult &&r) {
	const int slot = chunks_ ? chunks_->slot_of(r.chunk) : -1;
	if (slot < 0 || !chunks_) {
		if (chunks_) chunks_->note_discarded(r.chunk);
		return;
	}
	if (r.failed) {
		failures_++;
		UtilityFunctions::printerr("ColliderStreamer: readback failed for chunk (", r.chunk.x,
				", ", r.chunk.y, ", ", r.chunk.z, ")");
		chunks_->note_failed(r.chunk);
		return;
	}
	if (r.overflow && overflow_warnings_ < 8) {
		overflow_warnings_++;
		UtilityFunctions::push_warning("ColliderStreamer: chunk (", r.chunk.x, ", ", r.chunk.y,
				", ", r.chunk.z, ") hit a mesher cap; collider built from what fit");
	}

	PendingBuild pending;
	pending.slot = slot;
	pending.result = std::move(r);
	build_counts_[static_cast<size_t>(slot)]++;
	max_chunk_tris_ = std::max(max_chunk_tris_,
			static_cast<int>(pending.result.indices.size() / 3));

	// MeshResult indices originate in a GPU readback. Validate every index before the centroid
	// splitter can touch positions; a wrapped uint32_t must not become a negative signed check.
	// Keep complete valid triangles so a bad triangle cannot crash or poison the whole result.
	const int verts = static_cast<int>(pending.result.positions.size() / 3);
	const bool had_indices = !pending.result.indices.empty();
	std::vector<uint32_t> valid_indices;
	valid_indices.reserve(pending.result.indices.size());
	for (size_t t = 0; t + 2 < pending.result.indices.size(); t += 3) {
		bool valid = true;
		for (size_t k = 0; k < 3; k++) {
			if (pending.result.indices[t + k] >= static_cast<uint32_t>(verts)) {
				valid = false;
				break;
			}
		}
		if (valid) {
			valid_indices.push_back(pending.result.indices[t]);
			valid_indices.push_back(pending.result.indices[t + 1]);
			valid_indices.push_back(pending.result.indices[t + 2]);
		}
	}
	pending.result.indices.swap(valid_indices);
	if (pending.result.indices.empty() && had_indices) {
		failures_++;
		UtilityFunctions::printerr("ColliderStreamer: invalid mesh indices for chunk (",
				pending.result.chunk.x, ", ", pending.result.chunk.y, ", ",
				pending.result.chunk.z, "); keeping the previous collider and retrying");
		chunks_->note_failed(pending.result.chunk);
		return;
	}
	float origin[3];
	ve::chunk_world_origin(pending.result.chunk, origin);
	const float centre[3] = {origin[0] + ve::kChunkSize * 0.5f,
			origin[1] + ve::kChunkSize * 0.5f, origin[2] + ve::kChunkSize * 0.5f};
	ve::split_octants(pending.result.positions.data(), pending.result.indices.data(),
			static_cast<int>(pending.result.indices.size()), centre, pending.bins.data());
	bool populated = false;
	for (const auto &bin : pending.bins)
		if (!bin.empty()) populated = true;
	if (!populated && pending.result.overflow) {
		failures_++;
		UtilityFunctions::printerr("ColliderStreamer: overflowed chunk (", pending.result.chunk.x,
				", ", pending.result.chunk.y, ", ", pending.result.chunk.z,
				") produced no triangles; keeping the previous collider and retrying");
		chunks_->note_failed(pending.result.chunk);
		return;
	}
	pending_.push_back(std::move(pending));
}

int ColliderStreamer::run_frame(float cx, float cy, float cz) {
	return run_frame(cx, cy, cz, nullptr, 0);
}

int ColliderStreamer::run_frame(float cx, float cy, float cz, const float *extra_centers,
		int extra_count) {
	if (!chunks_ || !mesh_ || !mesh_->is_valid()) return 0;
	const Clock::time_point t_frame = Clock::now();
	last_plan_ms_ = 0.0f;
	last_apply_ms_ = 0.0f;
	last_submit_ms_ = 0.0f;
	int actions = 0;

	// 1. Land whatever the mesher thread has finished. Nothing here waits on the GPU.
	{
		std::vector<MeshResult> collected;
		mesh_->collect(&collected);
		for (MeshResult &r : collected) inbox_.push_back(std::move(r));
	}

	// 2. Split and build at most the configured number of octants. A PendingBuild owns fresh
	// shapes while it is in this queue. The old bodies remain in the space until all eight
	// octants have succeeded, so an edit cannot replace a live chunk with a half-collider.
	const Clock::time_point t_apply = Clock::now();
	last_faces_ms_ = 0.0f;
	last_setdata_ms_ = 0.0f;
	last_body_ms_ = 0.0f;
	last_tris_ = 0;
	last_build_ms_ = 0.0f;
	builds_last_frame_ = 0;
	while (builds_last_frame_ < max_builds_per_frame_) {
		if (pending_.empty()) {
			if (inbox_.empty()) break;
			enqueue_result(std::move(inbox_.front()));
			inbox_.pop_front();
			if (pending_.empty()) continue; // failed or stale result
		}
		PendingBuild &pending = pending_.front();
		if (chunks_->slot_of(pending.result.chunk) != pending.slot ||
				chunks_->chunk_of_slot(pending.slot) != pending.result.chunk ||
				chunks_->slot_state_of(pending.result.chunk) != ve::ChunkResidency::kBuilding) {
			discard_pending(pending);
			chunks_->note_discarded(pending.result.chunk);
			pending_.pop_front();
			continue;
		}
		if (pending.next_octant >= ve::kColliderOctants) {
			if (!commit_pending(pending)) {
				failures_++;
				chunks_->note_failed(pending.result.chunk);
				discard_pending(pending);
			}
			pending_.pop_front();
			actions++;
			continue;
		}
		const int octant = pending.next_octant++;
		const std::vector<uint32_t> &bin = pending.bins[static_cast<size_t>(octant)];
		if (bin.empty()) continue;
		if (builds_last_frame_ > 0) {
			const float est = static_cast<float>(bin.size() / 3) * kShapeBuildMsPerTriangle;
			if (ms_since(t_apply) + est > build_budget_ms_) {
				pending.next_octant--;
				break;
			}
		}
		const Clock::time_point t_build = Clock::now();
		const BuildOutcome outcome = build_octant(pending, octant);
		last_build_ms_ = std::max(last_build_ms_, ms_since(t_build));
		builds_last_frame_++;
		actions++;
		if (outcome == kFailed) {
			failures_++;
			chunks_->note_failed(pending.result.chunk);
			discard_pending(pending);
			pending_.pop_front();
		}
	}
	last_apply_ms_ = ms_since(t_apply);

	// 3. Plan. No new work while a batch or staged replacement is in flight.
	std::vector<float> centers;
	std::vector<float> radii;
	const int bubbles = std::max(0, extra_count);
	centers.reserve(3 * (1 + bubbles));
	radii.reserve(1 + bubbles);
	centers.push_back(cx);
	centers.push_back(cy);
	centers.push_back(cz);
	radii.push_back(chunks_->config().radius_m);
	if (extra_centers && bubbles > 0) {
		centers.insert(centers.end(), extra_centers, extra_centers + bubbles * 3);
		radii.insert(radii.end(), static_cast<size_t>(bubbles), bubble_radius_m_);
	}
	LogProbe probe;
	probe.gen = &gen_;
	probe.log = edit_log_;
	probe.mu = edit_mutex_;
	const int build_cap = (mesh_->busy() || !inbox_.empty() || !pending_.empty()) ? 0 : -1;
	const Clock::time_point t_plan = Clock::now();
	const ve::ChunkPlan plan = chunks_->update(centers.data(), radii.data(),
			static_cast<int>(centers.size() / 3), probe, build_cap);
	last_plan_ms_ = ms_since(t_plan);
	for (const auto &e : plan.releases) {
		release_slot(e.slot);
		actions++;
	}

	// 4. Mesh. Each request owns its copied op list and can only land after the staged queue
	// has drained, keeping one replacement transaction per chunk in flight.
	const Clock::time_point t_submit = Clock::now();
	if (!plan.builds.empty()) {
		std::vector<MeshRequest> requests;
		requests.reserve(plan.builds.size());
		{
			std::lock_guard<std::mutex> lock(*edit_mutex_);
			for (const auto &e : plan.builds) {
				requests.push_back({e.chunk, edit_log_->ops(ve::region_of_chunk(e.chunk))});
				last_submit_ops_[static_cast<size_t>(e.slot)] =
						static_cast<int>(requests.back().ops.size());
			}
		}
		const int n = static_cast<int>(requests.size());
		if (mesh_->submit(std::move(requests))) {
			actions += n;
		} else {
			for (const auto &e : plan.builds) chunks_->note_failed(e.chunk);
		}
	}
	last_submit_ms_ = ms_since(t_submit);
	last_frame_ms_ = ms_since(t_frame);
	return actions;
}
