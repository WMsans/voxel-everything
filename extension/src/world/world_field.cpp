#include "world/world_field.h"
#include "mesh/mesh_chunk.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <set>

namespace ve {

SnapshotSources::SnapshotSources(const FieldSourceSnapshot &s)
	: overrides(std::max(1, static_cast<int>(s.overrides.size()))) {
	ok = s.materialize(&overrides, &volumes);
}

FieldView WorldField::view() const {
	FieldView v;
	if (!valid()) return v;
	v.gen_ = gen_;
	v.log_ = log_;
	v.volumes_ = volumes_;
	v.overrides_ = overrides_;
	v.tables_ = tables_;
	v.seq_ = seq_;
	return v;
}

FieldView WorldField::lock() const {
	FieldView v = view();
	if (v.valid()) v.lock_ = std::unique_lock<std::mutex>(*mutex_);
	return v;
}

FieldView WorldField::locked_by_caller() const {
	return view();
}

bool WorldField::chunk_has_surface(IVec3 chunk) const {
	return lock().has_surface(chunk);
}

int WorldField::contact_samples(IVec3 cell, int axis, int face_samples) const {
	return lock().contact_samples(cell, axis, face_samples);
}

Sample FieldView::sample(float x, float y, float z) const {
	if (!valid()) return {};
	const std::vector<EditOp> &ops = log_->ops(region_of_point(x, y, z));
	return eval_field(*gen_, ops.data(), static_cast<int>(ops.size()), x, y, z, volumes_, overrides_);
}

bool FieldView::has_surface(IVec3 chunk) const {
	if (!valid()) return false;
	const std::vector<EditOp> &ops = log_->ops(region_of_chunk(chunk));
	return chunk_has_surface(*gen_, ops.data(), static_cast<int>(ops.size()), chunk, volumes_,
			overrides_);
}

int FieldView::contact_samples(IVec3 cell, int axis, int face_samples) const {
	if (!valid()) return 0;
	const std::vector<EditOp> &ops = log_->ops(region_of_brick(cell));
	return contact_samples_field(*gen_, ops.data(), static_cast<int>(ops.size()), cell, axis,
			face_samples, volumes_, overrides_);
}

RayHit FieldView::raycast(const float origin[3], const float dir[3], float max_dist) const {
	if (!valid()) return {};
	return ve::raycast(*gen_, *log_, origin, dir, max_dist, volumes_, overrides_);
}

// Moved verbatim from the old WorldStore helper (sub-project 5a).
bool FieldView::copy_sources(const std::vector<EditOp> &ops, IVec3 brick_lo, IVec3 brick_hi,
		FieldSourceSnapshot *out) const {
	if (!out || !overrides_) return false;
	out->overrides.clear();
	out->volumes.clear();
	for (int z = brick_lo.z; z <= brick_hi.z; z++)
		for (int y = brick_lo.y; y <= brick_hi.y; y++)
			for (int x = brick_lo.x; x <= brick_hi.x; x++) {
				IVec3 b{x, y, z};
				int slot = overrides_->slot_of(b);
				if (slot >= 0) {
					const OverrideBrick *data = overrides_->data(slot);
					if (!data) return false;
					if (!data->normal_oct.empty() && data->normal_oct.size() != kBrickSdfCount) return false;
					out->overrides.push_back({b, *data});
				}
			}
	std::set<int> seen;
	for (const auto &op : ops) {
		if (op.type != kOpVolumeAdd) continue;
		int slot = static_cast<int>(op.aux[0]);
		if (seen.count(slot)) continue;
		seen.insert(slot);
		const VolumeData *vd = volumes_->get(slot);
		if (!vd || !vd->valid()) return false;
		out->volumes.push_back({slot, *vd});
	}
	return true;
}

bool FieldView::snapshot_lattice(const float ops_lo[3], const float ops_hi[3],
		const float origin[3], float voxel, int dim, FieldSnapshot *out) const {
	if (!out) return false;
	*out = FieldSnapshot{};
	if (!valid()) return false;
	collect_ops_for_aabb(*log_, ops_lo, ops_hi, &out->ops);
	out->over_cap = out->ops.size() > static_cast<size_t>(kMaxRegionOps);
	out->edit_seq = seq_ ? seq_->load(std::memory_order_relaxed) : 0;
	if (tables_) {
		const IVec3 r = region_of_point(origin[0], origin[1], origin[2]);
		const auto it = tables_->find(std::tuple<int, int, int>{r.x, r.y, r.z});
		out->override_table = it == tables_->end() ? -1 : it->second;
	}
	const float span = static_cast<float>(dim - 1) * voxel;
	const IVec3 blo = brick_of_point(origin[0], origin[1], origin[2]);
	const IVec3 bhi = brick_of_point(origin[0] + span, origin[1] + span, origin[2] + span);
	return copy_sources(out->ops, blo, bhi, &out->sources);
}

bool FieldView::snapshot_region(IVec3 region, ConsolidationSnapshot *out) const {
	if (!out) return false;
	*out = ConsolidationSnapshot{};
	if (!valid() || !overrides_) return false;
	out->ops = log_->ops(region);
	const std::vector<uint64_t> &seqs = log_->seqs(region);
	out->through_seq = seqs.empty() ? 0 : seqs.back();
	plan_consolidation(out->ops.data(), static_cast<int>(out->ops.size()), region, &out->bricks);
	if (out->bricks.empty()) return true;
	IVec3 lo = out->bricks[0], hi = out->bricks[0];
	for (const IVec3 &b : out->bricks) {
		lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z);
		hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z);
	}
	return copy_sources(out->ops, lo, hi, &out->sources);
}

} // namespace ve
