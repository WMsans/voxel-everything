#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <memory>
#include "world/world_data.h"

namespace godot {

class GpuWorld;
class RaymarchPass;
class CompositePass;

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	bool use_local_device_ = false;
	Vector3i world_size_bricks_ = Vector3i(60, 20, 60);

	std::unique_ptr<ve::WorldData> world_;
	std::unique_ptr<GpuWorld> gpu_;
	// Deviation from brief: the brief declares unique_ptr<RaymarchPass>/<CompositePass>,
	// but those types are incomplete until Tasks 10/11, and std::unique_ptr requires a
	// complete type at VoxelWorld destructor instantiation. Raw pointers keep the same
	// accessor signatures; they stay null until Tasks 10/11.
	RaymarchPass *raymarch_pass_ = nullptr;   // Task 10
	CompositePass *composite_pass_ = nullptr; // Task 11
	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	bool initialized_ = false;

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _exit_tree() override;
	~VoxelWorld() override; // out-of-line: implicit one needs complete GpuWorld in every TU

	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	void set_world_size_bricks(Vector3i v) { world_size_bricks_ = v; }
	Vector3i get_world_size_bricks() const { return world_size_bricks_; }

	void ensure_initialized();
	bool is_initialized() const { return initialized_; }
	RenderingDevice *rd() const;

	GpuWorld *gpu_world() { return gpu_.get(); }
	RaymarchPass *raymarch_pass() { return raymarch_pass_; }
	CompositePass *composite_pass() { return composite_pass_; }

	Color debug_raymarch_pixel(Vector3 origin, Vector3 dir); // Task 10
	RID debug_indirection_tex() const;
	RID debug_sdf_atlas() const;
	RenderingDevice *debug_local_rd() const { return local_rd_; }
};

} // namespace godot
