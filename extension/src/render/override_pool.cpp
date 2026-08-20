#include "render/override_pool.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

namespace {
constexpr int kSdfWords = (ve::kBrickSdfCount + 3) / 4;
constexpr int kSdfStrideBytes = kSdfWords * 4;
constexpr int kMatWords = (ve::kBrickVoxelCount + 3) / 4;
constexpr int kMatStrideBytes = kMatWords * 4;
static_assert(ve::kBrickSdfCount == 4913, "update BRICK_SDF_COUNT in shaders/brick_layout.glslh");
static_assert(ve::kBrickVoxelCount == 4096, "update BRICK_VOXEL_COUNT in shaders/brick_layout.glslh");
static_assert(kSdfStrideBytes == 4916, "update override SDF byte stride in field.glslh");
static_assert(kMatStrideBytes == 4096, "update override material byte stride in field.glslh");

PackedByteArray filled_i32(int count, int value) {
	PackedByteArray b;
	b.resize(static_cast<int64_t>(count) * 4);
	int32_t *p = reinterpret_cast<int32_t *>(b.ptrw());
	for (int i = 0; i < count; i++) p[i] = value;
	return b;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}
} // namespace

OverridePool::~OverridePool() { teardown(); }

bool OverridePool::initialize(RenderingDevice *rd, int capacity, int max_region_slots) {
	teardown();
	if (!rd || capacity <= 0 || capacity > OverridePool::kDefaultCapacity || max_region_slots <= 0)
		return false;
	rd_ = rd;
	capacity_ = capacity;
	max_region_slots_ = max_region_slots;
	region_tables_.assign(static_cast<size_t>(max_region_slots), -1);
	const int64_t sdf_bytes = static_cast<int64_t>(capacity_) * kSdfStrideBytes;
	const int64_t mat_bytes = static_cast<int64_t>(capacity_) * kMatStrideBytes;
	PackedByteArray sdf_zero;
	sdf_zero.resize(sdf_bytes);
	sdf_zero.fill(255);
	sdf_ = rd_->storage_buffer_create(static_cast<uint32_t>(sdf_bytes), sdf_zero);
	PackedByteArray zero;
	zero.resize(mat_bytes);
	zero.fill(0);
	mat_ = rd_->storage_buffer_create(static_cast<uint32_t>(mat_bytes), zero);
	PackedByteArray table_zero = filled_i32(kMaxOverrideTables * ve::kRegionBrickCount, -1);
	tables_ = rd_->storage_buffer_create(static_cast<uint32_t>(table_zero.size()), table_zero);
	region_map_ = rd_->storage_buffer_create(static_cast<uint32_t>(max_region_slots_) * 4,
			filled_i32(max_region_slots_, -1));
	if (!is_valid()) {
		UtilityFunctions::printerr("OverridePool: resource creation failed");
		teardown();
		return false;
	}
	return true;
}

void OverridePool::teardown() {
	if (rd_) {
		free_if_valid(rd_, sdf_);
		free_if_valid(rd_, mat_);
		free_if_valid(rd_, tables_);
		free_if_valid(rd_, region_map_);
	}
	rd_ = nullptr;
	capacity_ = 0;
	max_region_slots_ = 0;
	region_tables_.clear();
}

bool OverridePool::upload(int slot, const ve::OverrideBrick &brick) {
	if (!rd_ || !is_valid() || slot < 0 || slot >= capacity_) return false;
	PackedByteArray sdf;
	sdf.resize(kSdfStrideBytes);
	sdf.fill(0);
	std::memcpy(sdf.ptrw(), brick.sdf, ve::kBrickSdfCount);
	rd_->buffer_update(sdf_, static_cast<uint32_t>(slot * kSdfStrideBytes), kSdfStrideBytes, sdf);
	PackedByteArray mat;
	mat.resize(kMatStrideBytes);
	std::memcpy(mat.ptrw(), brick.mat, ve::kBrickVoxelCount);
	rd_->buffer_update(mat_, static_cast<uint32_t>(slot * kMatStrideBytes), kMatStrideBytes, mat);
	return true;
}

void OverridePool::set_table_entry(RenderingDevice *rd, int table, int brick_index, int slot) {
	if (!rd || !tables_.is_valid() || table < 0 || table >= kMaxOverrideTables ||
			brick_index < 0 || brick_index >= ve::kRegionBrickCount || slot < -1 || slot >= capacity_)
		return;
	PackedByteArray b;
	b.resize(4);
	*reinterpret_cast<int32_t *>(b.ptrw()) = slot;
	rd->buffer_update(tables_, static_cast<uint32_t>((table * ve::kRegionBrickCount + brick_index) * 4), 4, b);
}

void OverridePool::set_region_table(RenderingDevice *rd, int region_slot, int table) {
	if (!rd || !region_map_.is_valid() || region_slot < 0 || region_slot >= max_region_slots_ ||
			table < -1 || table >= kMaxOverrideTables)
		return;
	region_tables_[static_cast<size_t>(region_slot)] = table;
	PackedByteArray b;
	b.resize(4);
	*reinterpret_cast<int32_t *>(b.ptrw()) = table;
	rd->buffer_update(region_map_, static_cast<uint32_t>(region_slot * 4), 4, b);
}

int OverridePool::region_table(int region_slot) const {
	return region_slot >= 0 && region_slot < static_cast<int>(region_tables_.size())
			? region_tables_[static_cast<size_t>(region_slot)] : -1;
}

void OverridePool::clear_table(RenderingDevice *rd, int table) {
	if (!rd || !tables_.is_valid() || table < 0 || table >= kMaxOverrideTables) return;
	rd->buffer_update(tables_, static_cast<uint32_t>(table * ve::kRegionBrickCount * 4),
			static_cast<uint32_t>(ve::kRegionBrickCount * 4),
			filled_i32(ve::kRegionBrickCount, -1));
}
