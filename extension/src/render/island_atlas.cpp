#include "render/island_atlas.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

// shaders/raymarch.comp.glsl hard-codes these (GLSL cannot include the header). A mismatch
// would read a neighbouring island's bytes rather than failing, so pin them here.
static_assert(ve::kIslandDim == 64, "update ISLAND_DIM in shaders/raymarch.comp.glsl");
static_assert(ve::kIslandVoxelCount == 262144, "update ISLAND_VOXELS in raymarch.comp.glsl");
static_assert(ve::kVolumeMipStride == 8, "update ISLAND_MIP_STRIDE in raymarch.comp.glsl");
static_assert(kMaxIslands == 32, "the tile mask is one uint per tile: 32 bits, 32 islands");

namespace {
constexpr int kMipCells = ve::kIslandDim / ve::kVolumeMipStride;             // 8
constexpr int kMipPerSlot = kMipCells * kMipCells * kMipCells;               // 512
constexpr int64_t kDescBytes = 128;                                          // 8 vec4
} // namespace

void IslandSlotDesc::recompute_world_aabb() {
	const float span = static_cast<float>(dim - 1) * voxel;
	for (int a = 0; a < 3; a++) {
		aabb_lo[a] = 1e30f;
		aabb_hi[a] = -1e30f;
	}
	for (int c = 0; c < 8; c++) {
		const float q[3] = {lattice_origin[0] + ((c & 1) ? span : 0.0f),
				lattice_origin[1] + ((c & 2) ? span : 0.0f),
				lattice_origin[2] + ((c & 4) ? span : 0.0f)};
		for (int a = 0; a < 3; a++) {
			// basis is COLUMN major: world_a = sum_k basis[k * 3 + a] * q[k].
			const float w = basis[0 * 3 + a] * q[0] + basis[1 * 3 + a] * q[1] +
					basis[2 * 3 + a] * q[2] + origin[a];
			aabb_lo[a] = std::min(aabb_lo[a], w);
			aabb_hi[a] = std::max(aabb_hi[a], w);
		}
	}
}

IslandAtlas::~IslandAtlas() {
	teardown();
}

bool IslandAtlas::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	if (!volumes_.initialize(rd, kMaxIslands, ve::kIslandDim)) {
		teardown();
		return false;
	}
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(kMaxIslands) * kMipPerSlot * 2);
	zero.fill(0);
	mip_ = rd->storage_buffer_create(static_cast<uint32_t>(zero.size()), zero);
	PackedByteArray descs;
	descs.resize(kMaxIslands * kDescBytes);
	descs.fill(0); // every slot dead: dim 0 is what the shader tests
	desc_ = rd->storage_buffer_create(static_cast<uint32_t>(descs.size()), descs);
	PackedByteArray ones;
	ones.resize(4);
	ones.fill(0xFF);
	fallback_mask_ = rd->storage_buffer_create(4, ones);
	if (!mip_.is_valid() || !desc_.is_valid() || !fallback_mask_.is_valid()) {
		UtilityFunctions::printerr("IslandAtlas: buffer creation failed");
		teardown();
		return false;
	}
	return true;
}

void IslandAtlas::teardown() {
	volumes_.teardown();
	if (rd_) {
		for (RID *r : {&mip_, &desc_, &fallback_mask_})
			if (r->is_valid()) rd_->free_rid(*r);
	}
	mip_ = RID();
	desc_ = RID();
	fallback_mask_ = RID();
	rd_ = nullptr;
	live_count_ = 0;
	for (bool &live : slot_live_) live = false;
}

bool IslandAtlas::upload(RenderingDevice *rd, int slot, const ve::VolumeData &data) {
	if (!rd || !is_valid() || slot < 0 || slot >= kMaxIslands) return false;
	if (!volumes_.upload(rd, slot, data)) return false;
	std::vector<uint8_t> mip;
	ve::build_volume_mip(data, &mip);
	if (static_cast<int>(mip.size()) != kMipPerSlot * 2) return false;
	PackedByteArray b;
	b.resize(static_cast<int64_t>(mip.size()));
	std::memcpy(b.ptrw(), mip.data(), mip.size());
	rd->buffer_update(mip_, static_cast<uint32_t>(slot * kMipPerSlot * 2),
			static_cast<uint32_t>(b.size()), b);
	return true;
}

void IslandAtlas::upload_descriptors(RenderingDevice *rd, const IslandSlotDesc *descs,
		int count) {
	if (!rd || !is_valid() || !descs) return;
	PackedByteArray b;
	b.resize(kMaxIslands * kDescBytes);
	b.fill(0);
	float *f = reinterpret_cast<float *>(b.ptrw());
	int32_t *i = reinterpret_cast<int32_t *>(b.ptrw());
	live_count_ = 0;
	for (bool &live : slot_live_) live = false;
	for (int s = 0; s < std::min(count, kMaxIslands); s++) {
		const IslandSlotDesc &d = descs[s];
		const int base = s * 32; // 32 floats per descriptor
		// Rows 0-2: the local->world basis columns, with the body translation in .w.
		for (int a = 0; a < 3; a++) {
			f[base + a * 4 + 0] = d.basis[a * 3 + 0];
			f[base + a * 4 + 1] = d.basis[a * 3 + 1];
			f[base + a * 4 + 2] = d.basis[a * 3 + 2];
			f[base + a * 4 + 3] = d.origin[a];
		}
		f[base + 12] = d.lattice_origin[0];
		f[base + 13] = d.lattice_origin[1];
		f[base + 14] = d.lattice_origin[2];
		f[base + 15] = d.voxel;
		i[base + 16] = d.live ? d.dim : 0; // dim 0 == dead, tested by the shader
		i[base + 17] = 0;
		i[base + 18] = 0;
		i[base + 19] = 0;
		slot_live_[s] = d.live;
		if (d.live) live_count_++;
		for (int a = 0; a < 3; a++) {
			f[base + 20 + a] = d.aabb_lo[a];
			f[base + 24 + a] = d.aabb_hi[a];
		}
	}
	rd->buffer_update(desc_, 0, static_cast<uint32_t>(b.size()), b);
}

void IslandAtlas::clear_slot(RenderingDevice *rd, int slot) {
	if (!rd || !is_valid() || slot < 0 || slot >= kMaxIslands) return;
	const bool was_live = slot_live_[slot];
	slot_live_[slot] = false;
	if (was_live) live_count_--;
	PackedByteArray b;
	b.resize(kDescBytes);
	b.fill(0);
	rd->buffer_update(desc_, static_cast<uint32_t>(slot * kDescBytes),
			static_cast<uint32_t>(kDescBytes), b);
}
