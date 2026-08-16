#include "render/volume_pool.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

// shaders/field.glslh hard-codes VOLUME_VOXELS (GLSL cannot include the header), and a
// mismatch would not fail anywhere -- it would silently read a neighbouring slot's bytes.
// Pin it here so changing the C++ constant breaks the BUILD, with the file that must follow
// named. (Same guard MeshPass uses for the chunk lattice.)
static_assert(ve::kIslandVoxelCount == 262144, "update VOLUME_VOXELS in shaders/field.glslh");
static_assert(ve::kIslandDim == 64, "update VOLUME_VOXELS in shaders/field.glslh");

VolumePool::~VolumePool() {
	teardown();
}

bool VolumePool::initialize(RenderingDevice *rd, int slots, int dim) {
	teardown();
	if (!rd || slots <= 0 || dim < 2) return false;
	rd_ = rd;
	slots_ = slots;
	dim_ = dim;
	const int64_t per_slot = static_cast<int64_t>(dim) * dim * dim;
	const int64_t bytes = per_slot * slots;
	PackedByteArray zero;
	zero.resize(bytes);
	// 255 is ve::encode_sdf(+0.64): an un-uploaded slot reads as solidly OUTSIDE everything,
	// so a stray reference to it can never add material. Zero would decode to -0.64 and
	// stamp a block of rock into the world.
	zero.fill(255);
	sdf_ = rd->storage_buffer_create(static_cast<uint32_t>(bytes), zero);
	zero.fill(0); // material 0 = air
	mat_ = rd->storage_buffer_create(static_cast<uint32_t>(bytes), zero);
	if (!is_valid()) {
		UtilityFunctions::printerr("VolumePool: buffer creation failed");
		teardown();
		return false;
	}
	return true;
}

void VolumePool::teardown() {
	if (rd_) {
		if (sdf_.is_valid()) rd_->free_rid(sdf_);
		if (mat_.is_valid()) rd_->free_rid(mat_);
	}
	sdf_ = RID();
	mat_ = RID();
	rd_ = nullptr;
	slots_ = 0;
	dim_ = 0;
}

bool VolumePool::upload(RenderingDevice *rd, int slot, const ve::VolumeData &data) {
	if (!rd || !is_valid() || slot < 0 || slot >= slots_) return false;
	if (data.dim != dim_ || data.empty()) return false;
	const int64_t per_slot = static_cast<int64_t>(dim_) * dim_ * dim_;
	if (static_cast<int64_t>(data.sdf.size()) != per_slot ||
			static_cast<int64_t>(data.mat.size()) != per_slot)
		return false;
	PackedByteArray b;
	b.resize(per_slot);
	std::memcpy(b.ptrw(), data.sdf.data(), static_cast<size_t>(per_slot));
	rd->buffer_update(sdf_, static_cast<uint32_t>(slot * per_slot),
			static_cast<uint32_t>(per_slot), b);
	std::memcpy(b.ptrw(), data.mat.data(), static_cast<size_t>(per_slot));
	rd->buffer_update(mat_, static_cast<uint32_t>(slot * per_slot),
			static_cast<uint32_t>(per_slot), b);
	return true;
}
