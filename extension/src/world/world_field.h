#pragma once
// ve::WorldField -- the one answer to "the world field here" (sub-project 5a,
// docs/superpowers/specs/2026-09-16-world-field-query-design.md). It hides which op list
// governs a point, the override and volume sources, the edit lock and the sequence stamp.
// A FieldView holds the edit mutex for its lifetime; snapshot_* are the only copying
// products, for GPU jobs and CPU references that run after the lock is released.
#include "connectivity/contact_refine.h"
#include "generator/volume_set.h"
#include "mesh/chunk_residency.h"
#include "world/edit_log.h"
#include "world/field_source_snapshot.h"
#include "world/override_store.h"
#include "world/raycast.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <tuple>
#include <vector>

namespace ve {

using OverrideTableMap = std::map<std::tuple<int, int, int>, int>;

// A lattice job's inputs: ops over the job's box AABB (not truncated), the override bricks
// and referenced volumes over the lattice's brick range, the table of the lattice origin's
// region and the edit sequence they were read at.
struct FieldSnapshot {
	std::vector<EditOp> ops;
	FieldSourceSnapshot sources;
	int override_table = -1;
	int64_t edit_seq = 0;
	bool over_cap = false; // ops.size() > kMaxRegionOps; the caller decides
};

// A consolidation bake's inputs.
struct ConsolidationSnapshot {
	std::vector<EditOp> ops;
	uint64_t through_seq = 0;
	std::vector<IVec3> bricks; // plan_consolidation order
	FieldSourceSnapshot sources;
};

// A snapshot made evaluable: what a GPU job received, as CPU stores.
struct SnapshotSources {
	explicit SnapshotSources(const FieldSourceSnapshot &s);
	OverrideStore overrides;
	VolumeSet volumes;
	bool ok = false;
};

class WorldField;

class FieldView {
public:
	FieldView(FieldView &&) = default;
	FieldView &operator=(FieldView &&) = default;
	FieldView(const FieldView &) = delete;
	FieldView &operator=(const FieldView &) = delete;

	bool valid() const { return gen_ != nullptr && log_ != nullptr; }
	Sample sample(float x, float y, float z) const;
	bool has_surface(IVec3 chunk) const;
	int contact_samples(IVec3 cell, int axis, int face_samples) const;
	RayHit raycast(const float origin[3], const float dir[3], float max_dist) const;
	// ops over [ops_lo, ops_hi] + kLatticeFilterPad; sources over the lattice
	// origin .. origin + (dim - 1) * voxel. False where a source is unavailable.
	bool snapshot_lattice(const float ops_lo[3], const float ops_hi[3], const float origin[3],
			float voxel, int dim, FieldSnapshot *out) const;
	bool snapshot_region(IVec3 region, ConsolidationSnapshot *out) const;

private:
	friend class WorldField;
	FieldView() = default;
	bool copy_sources(const std::vector<EditOp> &ops, IVec3 brick_lo, IVec3 brick_hi,
			FieldSourceSnapshot *out) const;

	const Generator *gen_ = nullptr;
	const EditLog *log_ = nullptr;
	const VolumeSet *volumes_ = nullptr;
	const OverrideStore *overrides_ = nullptr;
	const OverrideTableMap *tables_ = nullptr;
	const std::atomic<int64_t> *seq_ = nullptr;
	std::unique_lock<std::mutex> lock_;
};

class WorldField : public ChunkProbe, public ContactProbe {
public:
	WorldField() = default;
	WorldField(const Generator *gen, const EditLog *log, const VolumeSet *volumes,
			const OverrideStore *overrides, const OverrideTableMap *tables, std::mutex *edit_mutex,
			const std::atomic<int64_t> *edit_seq)
		: gen_(gen), log_(log), volumes_(volumes), overrides_(overrides), tables_(tables),
		  mutex_(edit_mutex), seq_(edit_seq) {}

	bool valid() const { return gen_ != nullptr && log_ != nullptr && volumes_ != nullptr && mutex_ != nullptr; }
	// Takes the edit mutex for the view's lifetime. An invalid field returns an invalid,
	// unlocked view.
	FieldView lock() const;
	// For code that already holds the edit mutex across more than this read.
	FieldView locked_by_caller() const;

	// Probe interfaces: one lock per call, as the adapters they replace.
	bool chunk_has_surface(IVec3 chunk) const override;
	int contact_samples(IVec3 cell, int axis, int face_samples) const override;

private:
	FieldView view() const;

	const Generator *gen_ = nullptr;
	const EditLog *log_ = nullptr;
	const VolumeSet *volumes_ = nullptr;
	const OverrideStore *overrides_ = nullptr;
	const OverrideTableMap *tables_ = nullptr;
	std::mutex *mutex_ = nullptr;
	const std::atomic<int64_t> *seq_ = nullptr;
};

} // namespace ve
