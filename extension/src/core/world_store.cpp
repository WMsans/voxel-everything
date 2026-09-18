#include "core/world_store.h"

namespace godot {

WorldStore::WorldStore(const ve::WorldConfig &config, ve::Generator *generator)
	: config_(config),
	  // Task 10: the field-generation seam is injected at construction. Owned from here
	  // on (see the header comment).
	  generator_(generator) {
	// The store is its own sink for the streamer handoff queue. Registered here because
	// nothing else exists yet: no lock is needed and none is held.
	pipeline_.add_sink(this);
}

WorldStore::~WorldStore() {
	delete generator_;
	generator_ = nullptr;
	release_cores();
}

ve::RegionWindow WorldStore::region_window() const {
	return residency_ ? residency_->window() : ve::RegionWindow{};
}

void WorldStore::set_generator(ve::Generator *generator) {
	if (generator == generator_) return;
	delete generator_; // pre-init only: nothing can hold the old seam mid-evaluation
	generator_ = generator;
}

void WorldStore::record(const ve::Invalidation &inv) {
	// The render thread's copy of the edit; WorldStreamer::run_frame swaps this queue out
	// under the same lock.
	if (inv.reason == ve::InvalidationReason::kEdit)
		pending_edits_.push_back({*inv.op, *inv.append});
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
	// The unbounded world means blocks accumulate with distance travelled. Dropping a distant
	// one is lossless: it re-reads as kCellUnknown and the mark pass refills it on return.
	occupancy_.evict_outside(center_[0], center_[1], center_[2], config_.occupancy_retention_m);
	return static_cast<int>(blocks.size());
}

int WorldStore::override_table_for_region(ve::IVec3 region) const {
	const auto it = override_tables_.find(std::tuple<int, int, int>{region.x, region.y, region.z});
	return it == override_tables_.end() ? -1 : it->second;
}

} // namespace godot
