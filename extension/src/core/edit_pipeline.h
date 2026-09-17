#pragma once
// EditPipeline — the one path an edit takes into the world, and the one place that tells every
// consumer about it (docs/superpowers/specs/2026-09-17-edit-pipeline-design.md).
//
// LOCK ORDER. This is the only place it is stated:
//
//	admission (g_voxel_compositor_admission_mutex) -> RenderOrchestrator::render_lifetime_mutex_
//	  -> WorldStore::edit_mutex()
//
// NOTHING NESTS INSIDE THE EDIT LOCK. apply(), preflight(), invalidate(), add_sink() and
// remove_sink() all run with the edit lock held, and a sink's record() only appends to that
// sink's own edit-lock-guarded queue -- it takes no lock of its own. The owner drains: it
// swaps its queue out under the edit lock, releases the lock, and only then acts (takes its
// own mutex, marks chunks, labels windows). That is what keeps LodSystem::mutex() and
// IslandManager's window bookkeeping off the edit path.
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "generator/edit_ops.h"
#include "world/edit_log.h"
#include "world/region.h"

namespace ve {

enum class InvalidationReason {
	kEdit,         // an op changed field state in at least one region
	kRejected,     // an op was refused: a full region list, or an oversized op
	kConsolidated, // a region's ops became override bricks; the field did not change
};

struct Invalidation {
	InvalidationReason reason = InvalidationReason::kEdit;
	// kEdit: the op's own world AABB. kConsolidated: the region's.
	float lo[3] = {0.0f, 0.0f, 0.0f};
	float hi[3] = {0.0f, 0.0f, 0.0f};
	IVec3 region{}; // kConsolidated only: sinks derive brick/chunk ranges from this, never
	                // from the float box, so no rounding can move a range.
	int64_t seq = 0; // the world edit sequence AFTER this op's bump
	const EditOp *op = nullptr;                    // kEdit, kRejected
	const EditLog::AppendResult *append = nullptr; // kEdit, kRejected
	bool notify_islands = true;

	static Invalidation consolidated(IVec3 region);
};

struct InvalidationSink {
	virtual ~InvalidationSink() = default;
	// Called with the edit lock HELD. Append to your own edit-lock-guarded queue and return;
	// take no other lock, and do no work a drain could do.
	virtual void record(const Invalidation &inv) = 0;
};

struct EditPolicy {
	// All-or-nothing: every op's regions are checked against the cap before anything is
	// appended, so a batch that is accepted is accepted whole.
	bool atomic = false;
	// False only for the island manager's own crumble carve (see EditSink's old comment):
	// the matter it removes was already labelled unanchored.
	bool notify_islands = true;
};

struct BatchResult {
	std::vector<EditLog::AppendResult> ops; // one per input op, in order
	bool refused = false;                   // atomic only: nothing was appended
	std::vector<IVec3> full;                // atomic only: the regions at the op cap. Empty
	                                        // when the refusal was a malformed or oversized op
};

class EditPipeline {
public:
	// Both pointers are to slots that outlive the pipeline: the log is created lazily and
	// released at exit, so the slot is read at every use.
	EditPipeline(EditLog *const *log, std::atomic<int64_t> *seq) : log_(log), seq_(seq) {}

	// Would this batch be accepted whole? Callers that must not half-apply (the island
	// manager's paste, its landing carve) ask this BEFORE they store, pin, upload or spawn,
	// then apply({.atomic = true}) under the same lock hold.
	bool preflight(std::span<const EditOp> ops, std::vector<IVec3> *full) const;
	// Caller holds WorldStore::edit_mutex().
	BatchResult apply(std::span<const EditOp> ops, EditPolicy policy);
	// Consolidation's commit: the base bytes changed without an op. Caller holds the lock.
	void invalidate(const Invalidation &inv);
	// Caller holds the lock, and holds no other lock.
	void add_sink(InvalidationSink *sink);
	void remove_sink(InvalidationSink *sink);

private:
	void record(const Invalidation &inv);

	EditLog *const *log_ = nullptr;
	std::atomic<int64_t> *seq_ = nullptr;
	std::vector<InvalidationSink *> sinks_;
};

// How many boxes a deferred sink's queue holds before it folds (see merge_or_cap).
inline constexpr size_t kInvalidationQueueCap = 1024;

// An inclusive box, in whatever units the sink queues: world metres for LoD marks, chunk
// coordinates for the collider remesh queue.
template <typename T>
struct Box3 {
	T lo[3];
	T hi[3];
};

// Queue `box`, merging it into every queued box it overlaps, so a sink's queue stays bounded
// by the number of DISJOINT areas edited rather than by the number of edits. Over-covering is
// safe for every consumer here (it re-marks ground that did not change); losing a box is not.
template <typename T>
void merge_or_cap(std::vector<Box3<T>> *queue, Box3<T> box, size_t cap = kInvalidationQueueCap) {
	if (!queue) return;
	const auto overlaps = [](const Box3<T> &a, const Box3<T> &b) {
		for (int k = 0; k < 3; k++)
			if (a.lo[k] > b.hi[k] || a.hi[k] < b.lo[k]) return false;
		return true;
	};
	const auto unite = [](Box3<T> *a, const Box3<T> &b) {
		for (int k = 0; k < 3; k++) {
			if (b.lo[k] < a->lo[k]) a->lo[k] = b.lo[k];
			if (b.hi[k] > a->hi[k]) a->hi[k] = b.hi[k];
		}
	};
	// A box can bridge two queued boxes that did not overlap each other, so restart after
	// each absorption until nothing more overlaps.
	for (size_t i = 0; i < queue->size();) {
		if (!overlaps((*queue)[i], box)) {
			i++;
			continue;
		}
		unite(&box, (*queue)[i]);
		(*queue)[i] = queue->back();
		queue->pop_back();
		i = 0;
	}
	queue->push_back(box);
	if (queue->size() <= cap) return;
	// ponytail: past the cap everything folds into one bounding box, which can mark a huge
	// span dirty. Reaching the cap needs a drain that is not running, and the fold costs
	// rebuilds rather than correctness. If it is ever reached with a live drain, fold
	// pairwise by smallest growth instead.
	Box3<T> all = (*queue)[0];
	for (const Box3<T> &b : *queue) unite(&all, b);
	queue->assign(1, all);
}

} // namespace ve
