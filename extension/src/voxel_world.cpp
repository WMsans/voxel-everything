#include "voxel_world.h"
#include "render/gpu_world.h"
#include "render/camera_params.h"
#include "render/raymarch_pass.h"
#include "generator/generator.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
	ClassDB::bind_method(D_METHOD("set_world_size_bricks", "v"), &VoxelWorld::set_world_size_bricks);
	ClassDB::bind_method(D_METHOD("get_world_size_bricks"), &VoxelWorld::get_world_size_bricks);
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("debug_raymarch_pixel", "origin", "dir"), &VoxelWorld::debug_raymarch_pixel);
	ClassDB::bind_method(D_METHOD("debug_indirection_tex"), &VoxelWorld::debug_indirection_tex);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelWorld::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelWorld::debug_local_rd);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_bricks"), "set_world_size_bricks", "get_world_size_bricks");
}

void VoxelWorld::_ready() {}

VoxelWorld::~VoxelWorld() {}

void VoxelWorld::_exit_tree() {
	// Delete the raymarch pass while the device is still valid: ~RaymarchPass() frees
	// its RIDs on rd(), so it must run before GpuWorld teardown and device destruction.
	if (raymarch_pass_) {
		delete raymarch_pass_;
		raymarch_pass_ = nullptr;
	}
	if (gpu_) gpu_->teardown();
	if (local_rd_) {
		// Brief used local_rd_->free(); godot-cpp master has no no-arg free() on
		// RenderingDevice (only the macro's static free), so free via memdelete.
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
}

RenderingDevice *VoxelWorld::rd() const {
	return use_local_device_ ? local_rd_ : main_rd_;
}

void VoxelWorld::ensure_initialized() {
	if (initialized_) return;
	if (use_local_device_ && !local_rd_) {
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	} else if (!use_local_device_) {
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	}
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	world_ = std::make_unique<ve::WorldData>(world_size_bricks_.x, world_size_bricks_.y, world_size_bricks_.z);
	ve::AnalyticGenerator gen;
	world_->generate(gen);
	UtilityFunctions::print("VoxelWorld: generated ", world_->active_brick_count(), " bricks");
	gpu_ = std::make_unique<GpuWorld>();
	if (!gpu_->initialize(device, *world_)) {
		gpu_.reset();
		return;
	}
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	initialized_ = true;
}

RID VoxelWorld::debug_indirection_tex() const { return gpu_ ? gpu_->indirection_tex() : RID(); }
RID VoxelWorld::debug_sdf_atlas() const { return gpu_ ? gpu_->sdf_atlas() : RID(); }

// Half-precision to single-precision (normal + subnormal paths).
static float half_to_float(uint16_t v) {
	const uint32_t sign = (v & 0x8000u) << 16;
	const uint32_t exp = (v >> 10) & 0x1F;
	const uint32_t mant = v & 0x3FF;
	if (exp == 0) return (sign ? -1.0f : 1.0f) * mant / 1024.0f / 16384.0f;
	uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	// Deviation 3: initialized_ is not reset on _exit_tree, so after remove/re-add rd()
	// can be stale/null; guard everything (gpu_ additionally null-checked).
	if (!initialized_ || !device || !gpu_ || !raymarch_pass_) return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.dims[0] = world_size_bricks_.x;
	cam.dims[1] = world_size_bricks_.y;
	cam.dims[2] = world_size_bricks_.z;
	if (!raymarch_pass_->render(device, *gpu_, cam, 1, 1)) return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (data.size() < 8) return Color(1, 0, 1);
	const uint16_t *h = reinterpret_cast<const uint16_t *>(data.ptr());
	return Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
}
