#include "voxel_world.h"
#include "render/gpu_world.h"
#include "generator/generator.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
	ClassDB::bind_method(D_METHOD("set_world_size_bricks", "v"), &VoxelWorld::set_world_size_bricks);
	ClassDB::bind_method(D_METHOD("get_world_size_bricks"), &VoxelWorld::get_world_size_bricks);
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("debug_indirection_tex"), &VoxelWorld::debug_indirection_tex);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelWorld::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelWorld::debug_local_rd);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_bricks"), "set_world_size_bricks", "get_world_size_bricks");
}

void VoxelWorld::_ready() {}

VoxelWorld::~VoxelWorld() {}

void VoxelWorld::_exit_tree() {
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
	initialized_ = true;
}

RID VoxelWorld::debug_indirection_tex() const { return gpu_ ? gpu_->indirection_tex() : RID(); }
RID VoxelWorld::debug_sdf_atlas() const { return gpu_ ? gpu_->sdf_atlas() : RID(); }

// TEMPORARY: Task 10 replaces this with a real raymarch readback.
Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	return Color(1, 0, 1);
}
