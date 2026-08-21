#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <cstdint>
#include <map>
#include "generator/volume_set.h"
#include "world/brick.h"
#include "world/normal_range_allocator.h"

namespace godot {

// Telemetry for one StoredNormalPool. capacity_bytes is the TOTAL device budget (packed
// payload plus BOTH offset tables); live_bytes/high_water_bytes report packed-payload use.
// allocation_failures counts refused NormalRangeAllocator requests; fallback_hits is a
// CPU-side count of render-reachable sources published with normal offset -1 because
// their payload was absent, malformed, or unallocatable -- incremented ONCE when such a
// source enters fallback, never per frame it remains resident (no per-pixel atomics, no
// render-target readback).
struct StoredNormalStats {
	uint32_t capacity_bytes = 0;
	uint32_t live_bytes = 0;
	uint32_t high_water_bytes = 0;
	uint64_t allocation_failures = 0;
	uint64_t fallback_hits = 0;
};

// The render device's compact-normal store: one buffer of packed uint16 oct normals plus
// two signed-offset tables (per volume slot, per override-brick slot). A table entry of
// -1 means "no normals bound; fall back to differentiating the R8 atlas" -- the fail-soft
// path that can never reject geometry or edits.
//
// budget_bytes is the HARD total across all three buffers: kMaxVolumes*4 bytes of volume
// offsets, override_capacity*4 bytes of override offsets, and every four-byte-aligned
// byte of the remainder goes to packed normals. Initialization is rejected when the
// metadata alone does not fit, so the pool never grows implicitly.
class StoredNormalPool {
public:
	// The global constraint: 32 MiB including both offset tables, at default settings.
	static constexpr uint32_t kDefaultBudgetBytes = 32u * 1024u * 1024u;

	StoredNormalPool() = default;
	~StoredNormalPool();
	StoredNormalPool(const StoredNormalPool &) = delete;
	StoredNormalPool &operator=(const StoredNormalPool &) = delete;

	bool initialize(RenderingDevice *rd, uint32_t budget_bytes, int max_volumes,
			int override_capacity);
	void teardown();
	bool is_valid() const { return normals_.is_valid(); }

	RID normal_buffer() const { return normals_; }
	RID volume_offsets_buffer() const { return volume_offsets_; }
	RID override_offsets_buffer() const { return override_offsets_; }
	uint32_t capacity_bytes() const { return capacity_; }

	// Device-level commands: record BEFORE compute_list_begin, never inside an open list.

	// Uploads one volume's packed normals and publishes its byte offset in the volume
	// table. A source without a complete dim^3 normal_oct lattice enters fallback: the
	// table entry stays/parks at -1 exactly once per upload. An equal-sized live span is
	// updated in place; otherwise the new span is allocated BEFORE the old handle is
	// released so an allocation failure preserves the valid old bytes. Returns the
	// published byte offset, or -1 on fallback.
	int64_t upload_volume(RenderingDevice *rd, int slot, const ve::VolumeData &data);
	void release_volume(RenderingDevice *rd, int slot);

	// Same contract for one override brick's kBrickSdfCount packed samples. Returns the
	// published byte offset, or -1 on fallback.
	int64_t upload_override(RenderingDevice *rd, int slot, const uint16_t *packed_normals,
			int count);
	void release_override(RenderingDevice *rd, int slot);

	StoredNormalStats stats() const;

private:
	static constexpr int64_t kNoOffset = -1;

	int64_t upload(RenderingDevice *rd, int slot, const void *packed_bytes,
			int64_t byte_count, int expected_count, bool is_volume);

	RenderingDevice *rd_ = nullptr;
	RID normals_, volume_offsets_, override_offsets_;
	uint32_t capacity_ = 0;      // total budget across all THREE buffers
	uint32_t payload_bytes_ = 0;  // four-byte-aligned remainder handed to the allocator
	int max_volumes_ = 0;
	int override_capacity_ = 0;
	ve::NormalRangeAllocator allocator_;
	std::map<int, ve::NormalAllocation> volume_live_;
	std::map<int, ve::NormalAllocation> override_live_;
	StoredNormalStats stats_{};
};

} // namespace godot
