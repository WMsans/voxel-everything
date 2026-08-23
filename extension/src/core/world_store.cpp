#include "core/world_store.h"

namespace godot {

WorldStore::WorldStore(const ve::WorldConfig &config, ve::FieldGenerator *generator)
	: config_(config),
	  // Task 10: the field-generation seam is injected at construction; a null pointer
	  // means "today's terrain". Owned from here on (see the header comment).
	  generator_(generator ? generator : new ve::ProceduralFieldGenerator()) {}

WorldStore::~WorldStore() {
	delete generator_;
	generator_ = nullptr;
	release_cores();
}

void WorldStore::set_generator(ve::FieldGenerator *generator) {
	if (generator == generator_) return;
	delete generator_; // pre-init only: nothing can hold the old seam mid-evaluation
	generator_ = generator ? generator : new ve::ProceduralFieldGenerator();
}

ve::EditLog::AppendResult WorldStore::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(edit_mutex_);
	return append_edit_locked(op);
}

ve::EditLog::AppendResult WorldStore::append_edit_locked(const ve::EditOp &op,
		bool notify_islands) {
	if (!edit_log_) return {};
	ve::EditLog::AppendResult r = edit_log_->append(op);
	// Queue before the list reaches its hard cap. The bake is asynchronous, so the spare 64
	// entries absorb edits appended while the worker is in flight.
	for (const ve::IVec3 &region : r.touched)
		if (edit_log_->op_count(region) >= ve::kConsolidateAtOps)
			consolidation_sink_->queue_consolidation(region);
	// Bump AFTER the append and under the same lock the streamer uses to capture op counts.
	// If the seq moved before the append, a readback stamped between the bump and the append
	// would claim edits that are not in the GPU state the readback describes.
	bump_edit_seq();
	// Connectivity's half of the fan-out. Runs under the append lock; the manager's
	// pending-window queue is guarded by its own windows_mutex_ (note_edit may be called
	// from a tool thread), and the seq bump above lets the window know which readback is
	// "new enough" to act on. A fully rejected op changed no field state, so it must not
	// enqueue a window: doing so would re-label the same component and retry the rejected
	// edit forever. (The touched-empty gate lives here because only this body sees `r`;
	// the sink's `notify_islands` argument keeps the caller's intent for the adapter to
	// re-check alongside its own island-manager presence check.)
	if (notify_islands && !r.touched.empty())
		edit_sink_->on_edit_appended(op, notify_islands);
	pending_edits_.push_back({op, r});
	return r;
}

int64_t WorldStore::bump_edit_seq() {
	return edit_seq_.fetch_add(1, std::memory_order_relaxed);
}

int WorldStore::drain_occupancy() {
	std::vector<OccupancyBlock> blocks;
	{
		std::lock_guard<std::mutex> lock(occupancy_mutex_);
		blocks.swap(occupancy_inbox_);
	}
	for (const OccupancyBlock &b : blocks) {
		// A region marked in consecutive frames can have two reads in flight, and the older
		// one can land after the newer one. Never let it regress the grid or the block's seq.
		if (b.seq < occupancy_.block_seq(b.region)) continue;
		occupancy_.set_block(b.region, b.bytes.data(), b.seq);
	}
	return static_cast<int>(blocks.size());
}

int WorldStore::override_table_for_region(ve::IVec3 region) const {
	const auto it = override_tables_.find(std::tuple<int, int, int>{region.x, region.y, region.z});
	return it == override_tables_.end() ? -1 : it->second;
}

} // namespace godot
