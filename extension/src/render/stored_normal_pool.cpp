#include "render/stored_normal_pool.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <cstring>

using namespace godot;

namespace {

// A freshly created Vulkan buffer's bytes are only defined where something wrote them,
// so the payload starts as explicit zeros rather than an empty array.
PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

PackedByteArray filled_i32(int count, int32_t value) {
	PackedByteArray b;
	b.resize(static_cast<int64_t>(count) * 4);
	int32_t *p = reinterpret_cast<int32_t *>(b.ptrw());
	for (int i = 0; i < count; i++) p[i] = value;
	return b;
}

void publish_offset(RenderingDevice *rd, RID table, int slot, int64_t byte_offset) {
	PackedByteArray b;
	b.resize(4);
	*reinterpret_cast<int32_t *>(b.ptrw()) = static_cast<int32_t>(byte_offset);
	rd->buffer_update(table, static_cast<uint32_t>(slot) * 4, 4, b);
}

} // namespace

StoredNormalPool::~StoredNormalPool() {
	teardown();
}

bool StoredNormalPool::initialize(RenderingDevice *rd, uint32_t budget_bytes,
		int max_volumes, int override_capacity) {
	teardown();
	if (!rd || max_volumes <= 0 || override_capacity <= 0) return false;
	const uint64_t metadata =
			static_cast<uint64_t>(max_volumes) * 4 + static_cast<uint64_t>(override_capacity) * 4;
	if (metadata > budget_bytes) return false; // metadata must fit: the pool never grows
	capacity_ = budget_bytes - static_cast<uint32_t>(
			static_cast<uint64_t>(budget_bytes) % 4);
	const uint32_t raw_payload = capacity_ - static_cast<uint32_t>(metadata);
	payload_bytes_ = raw_payload - raw_payload % 4;

	rd_ = rd;
	max_volumes_ = max_volumes;
	override_capacity_ = override_capacity;
	allocator_ = ve::NormalRangeAllocator(payload_bytes_);
	stats_ = StoredNormalStats{};
	stats_.capacity_bytes = capacity_;

	normals_ = rd->storage_buffer_create(static_cast<uint32_t>(payload_bytes_),
			zeroed(static_cast<int64_t>(payload_bytes_)));
	volume_offsets_ = rd->storage_buffer_create(static_cast<uint32_t>(max_volumes_) * 4,
			filled_i32(max_volumes_, -1));
	override_offsets_ = rd->storage_buffer_create(
			static_cast<uint32_t>(override_capacity_) * 4, filled_i32(override_capacity_, -1));
	if (!normals_.is_valid() || !volume_offsets_.is_valid() || !override_offsets_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void StoredNormalPool::teardown() {
	// The offset tables and payload reference the same device as every consumer's uniform
	// set; freeing them here (before GpuAtlas tears down anything else that reads them)
	// keeps one teardown order in one place.
	volume_live_.clear();
	override_live_.clear();
	allocator_ = ve::NormalRangeAllocator(0);
	if (!rd_) return;
	for (RID *r : {&normals_, &volume_offsets_, &override_offsets_})
		if (r->is_valid()) rd_->free_rid(*r);
	normals_ = RID();
	volume_offsets_ = RID();
	override_offsets_ = RID();
	rd_ = nullptr;
}

int64_t StoredNormalPool::upload_volume(RenderingDevice *rd, int slot,
		const ve::VolumeData &data) {
	if (!data.has_normals()) {
		// Payload absent: this render-reachable source enters fallback.
		if (rd && rd_ && slot >= 0 && slot < max_volumes_) {
			release_volume(rd, slot); // publishes -1 first, then frees the old span
			stats_.fallback_hits++;
		}
		return kNoOffset;
	}
	const int expected = data.voxel_count();
	return upload(rd, slot, data.normal_oct.data(),
			static_cast<int64_t>(data.normal_oct.size()) * 2, expected, true);
}

void StoredNormalPool::release_volume(RenderingDevice *rd, int slot) {
	if (!rd || !is_valid() || slot < 0 || slot >= max_volumes_) return;
	publish_offset(rd, volume_offsets_, slot, kNoOffset); // -1 BEFORE returning the range
	const auto it = volume_live_.find(slot);
	if (it == volume_live_.end()) return;
	allocator_.release(it->second);
	volume_live_.erase(it);
}

int64_t StoredNormalPool::upload_override(RenderingDevice *rd, int slot,
		const uint16_t *packed_normals, int count) {
	return upload(rd, slot, packed_normals, static_cast<int64_t>(count) * 2,
			ve::kBrickSdfCount, false);
}

void StoredNormalPool::release_override(RenderingDevice *rd, int slot) {
	if (!rd || !is_valid() || slot < 0 || slot >= override_capacity_) return;
	publish_offset(rd, override_offsets_, slot, kNoOffset);
	const auto it = override_live_.find(slot);
	if (it == override_live_.end()) return;
	allocator_.release(it->second);
	override_live_.erase(it);
}

int64_t StoredNormalPool::upload(RenderingDevice *rd, int slot, const void *packed_bytes,
		int64_t byte_count, int expected_count, bool is_volume) {
	if (!rd || !is_valid()) return kNoOffset;
	const bool volume_ok = is_volume && slot >= 0 && slot < max_volumes_;
	const bool override_ok = !is_volume && slot >= 0 && slot < override_capacity_;
	if (!volume_ok && !override_ok) return kNoOffset;
	std::map<int, ve::NormalAllocation> &live = is_volume ? volume_live_ : override_live_;

	// Sample-count validation FIRST: a malformed payload enters fallback rather than
	// writing a torn lattice or a wrong-length span.
	if (expected_count <= 0 || packed_bytes == nullptr ||
			byte_count != static_cast<int64_t>(expected_count) * 2) {
		if (is_volume) release_volume(rd, slot);
		else release_override(rd, slot);
		stats_.fallback_hits++;
		return kNoOffset;
	}

	const uint32_t bytes = static_cast<uint32_t>(byte_count);
	const auto existing = live.find(slot);
	if (existing != live.end() && existing->second.size == bytes) {
		// Equal-sized live allocation: reuse the span in place, no allocator churn.
		PackedByteArray b;
		b.resize(byte_count);
		std::memcpy(b.ptrw(), packed_bytes, static_cast<size_t>(byte_count));
		rd->buffer_update(normals_, existing->second.offset, bytes, b);
		return existing->second.offset;
	}

	// Allocate BEFORE releasing the old handle so an allocation failure preserves the
	// valid old bytes under the already-published offset.
	const ve::NormalAllocation alloc = allocator_.allocate(bytes);
	if (!alloc.valid()) {
		stats_.allocation_failures++;
		if (existing != live.end())
			return existing->second.offset; // stale-but-valid normals stay bound
		publish_offset(rd, is_volume ? volume_offsets_ : override_offsets_, slot, kNoOffset);
		stats_.fallback_hits++; // unallocatable and nothing valid to keep: enter fallback
		return kNoOffset;
	}
	if (existing != live.end()) {
		// The new span exists; the old handle can go. Its published entry is about to be
		// overwritten, so no separate -1 publication is needed here.
		allocator_.release(existing->second);
		live.erase(existing);
	}

	PackedByteArray b;
	b.resize(byte_count);
	std::memcpy(b.ptrw(), packed_bytes, static_cast<size_t>(byte_count));
	// Payload bytes first, THEN the table entry: the shader never sees an offset whose
	// bytes are not already on the device.
	rd->buffer_update(normals_, alloc.offset, bytes, b);
	publish_offset(rd, is_volume ? volume_offsets_ : override_offsets_, slot, alloc.offset);
	live[slot] = alloc;
	return alloc.offset;
}

StoredNormalStats StoredNormalPool::stats() const {
	StoredNormalStats s = stats_;
	s.live_bytes = allocator_.used_bytes();
	s.high_water_bytes = allocator_.high_water_bytes();
	return s;
}
