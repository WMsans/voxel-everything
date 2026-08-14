#include "render/gpu_world.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

static void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

void GpuWorld::teardown() {
	free_if_valid(rd_, sdf_atlas_);
	free_if_valid(rd_, mat_atlas_);
	free_if_valid(rd_, indirection_);
	free_if_valid(rd_, palette_);
	rd_ = nullptr;
}

bool GpuWorld::initialize(RenderingDevice *rd, const ve::WorldData &world) {
	rd_ = rd;
	const int active = world.active_brick_count();
	if (active <= 0 || active > kAtlasBrickCount) {
		UtilityFunctions::printerr("GpuWorld: active brick count ", active, " out of atlas range");
		return false;
	}

	// The two atlases have DIFFERENT strides. The SDF is a lattice field carrying a
	// one-voxel apron on each brick's positive faces (ve::kBrickSdfStride == 17) so the
	// shader's trilinear filter can cover the brick's full extent; materials are a plain
	// 16^3 cell grid read with nearest filtering and need no apron.
	const int sdf_w = kAtlasBricksX * ve::kBrickSdfStride; // 544
	const int sdf_h = kAtlasBricksY * ve::kBrickSdfStride; // 272
	const int sdf_d = kAtlasBricksZ * ve::kBrickSdfStride; // 544
	const int mat_w = kAtlasBricksX * ve::kBrickVoxels; // 512
	const int mat_h = kAtlasBricksY * ve::kBrickVoxels; // 256
	const int mat_d = kAtlasBricksZ * ve::kBrickVoxels; // 512

	// On Godot 4.7.1 a 3D texture has exactly one layer, so texture_create expects a
	// single full-volume PackedByteArray (width*height*depth*bytes), tightly packed in
	// (x, y, z) order with x fastest. texture_get_data returns the same full volume.
	PackedByteArray sdf_vol, mat_vol;
	sdf_vol.resize(sdf_w * sdf_h * sdf_d);
	sdf_vol.fill(0);
	mat_vol.resize(mat_w * mat_h * mat_d);
	mat_vol.fill(0);
	for (int slot = 0; slot < active; slot++) {
		const ve::Brick &b = world.brick(slot);
		const int sx = slot % kAtlasBricksX;
		const int sy = (slot / kAtlasBricksX) % kAtlasBricksY;
		const int sz = slot / (kAtlasBricksX * kAtlasBricksY);
		for (int vz = 0; vz < ve::kBrickSdfStride; vz++) {
			const int az = sz * ve::kBrickSdfStride + vz;
			for (int vy = 0; vy < ve::kBrickSdfStride; vy++) {
				const int ay = sy * ve::kBrickSdfStride + vy;
				for (int vx = 0; vx < ve::kBrickSdfStride; vx++) {
					const int ax = sx * ve::kBrickSdfStride + vx;
					sdf_vol[ax + ay * sdf_w + az * sdf_w * sdf_h] = b.sdf[ve::sdf_index(vx, vy, vz)];
				}
			}
		}
		for (int vz = 0; vz < ve::kBrickVoxels; vz++) {
			const int az = sz * ve::kBrickVoxels + vz;
			for (int vy = 0; vy < ve::kBrickVoxels; vy++) {
				const int ay = sy * ve::kBrickVoxels + vy;
				for (int vx = 0; vx < ve::kBrickVoxels; vx++) {
					const int ax = sx * ve::kBrickVoxels + vx;
					mat_vol[ax + ay * mat_w + az * mat_w * mat_h] =
							ve::get_mat_index(b, ve::voxel_index(vx, vy, vz));
				}
			}
		}
	}
	TypedArray<PackedByteArray> sdf_ta, mat_ta;
	sdf_ta.push_back(sdf_vol);
	mat_ta.push_back(mat_vol);

	Ref<RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
	fmt->set_mipmaps(1);
	fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	fmt->set_width(sdf_w);
	fmt->set_height(sdf_h);
	fmt->set_depth(sdf_d);
	fmt->set_format(RenderingDevice::DATA_FORMAT_R8_UNORM);
	sdf_atlas_ = rd->texture_create(fmt, view, sdf_ta);
	fmt->set_width(mat_w);
	fmt->set_height(mat_h);
	fmt->set_depth(mat_d);
	fmt->set_format(RenderingDevice::DATA_FORMAT_R8_UINT);
	mat_atlas_ = rd->texture_create(fmt, view, mat_ta);

	// --- indirection (R32_SINT 3D, world dims) ---
	const ve::Dims d = world.dims();
	Ref<RDTextureFormat> ifmt;
	ifmt.instantiate();
	ifmt->set_format(RenderingDevice::DATA_FORMAT_R32_SINT);
	ifmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
	ifmt->set_width(d.x);
	ifmt->set_height(d.y);
	ifmt->set_depth(d.z);
	ifmt->set_mipmaps(1);
	ifmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	TypedArray<PackedByteArray> ind_ta;
	PackedByteArray ind_bytes;
	ind_bytes.resize(d.x * d.y * d.z * 4);
	int32_t *ptr = reinterpret_cast<int32_t *>(ind_bytes.ptrw());
	for (int i = 0; i < d.x * d.y * d.z; i++) ptr[i] = world.indirection()[i];
	ind_ta.push_back(ind_bytes);
	indirection_ = rd->texture_create(ifmt, view, ind_ta);

	// --- palette storage buffer ---
	PackedByteArray pal_bytes;
	pal_bytes.resize(kAtlasBrickCount * ve::kBrickPaletteSize * 2);
	pal_bytes.fill(0);
	uint16_t *pal_ptr = reinterpret_cast<uint16_t *>(pal_bytes.ptrw());
	for (int slot = 0; slot < active; slot++)
		for (int p = 0; p < ve::kBrickPaletteSize; p++)
			pal_ptr[slot * ve::kBrickPaletteSize + p] = world.brick(slot).palette[p];
	palette_ = rd->storage_buffer_create(pal_bytes.size(), pal_bytes);

	const bool ok = sdf_atlas_.is_valid() && mat_atlas_.is_valid() && indirection_.is_valid() && palette_.is_valid();
	if (!ok) {
		UtilityFunctions::printerr("GpuWorld: resource creation failed");
		teardown();
	}
	return ok;
}
