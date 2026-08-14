#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <memory>
#include "world/region.h"
#include "world/world_data.h"

namespace godot {

class GpuAtlas;
class GpuWorld;
class RaymarchPass;
class CompositePass;

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	bool use_local_device_ = false;
	Vector3i world_size_bricks_ = Vector3i(60, 20, 60);
	Vector3i atlas_bricks_ = Vector3i(64, 32, 32);
	int max_region_slots_ = 512;
	int max_brick_jobs_ = 16384;
	Vector3i world_origin_bricks_ = Vector3i(0, -64, 0);
	Vector3i world_size_regions_ = Vector3i(64, 8, 64);
	float residency_radius_m_ = 96.0f;
	GpuAtlas *atlas_ = nullptr;

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
	void set_atlas_bricks(Vector3i v) { atlas_bricks_ = v; }
	Vector3i get_atlas_bricks() const { return atlas_bricks_; }
	void set_max_region_slots(int v) { max_region_slots_ = v; }
	int get_max_region_slots() const { return max_region_slots_; }
	void set_max_brick_jobs(int v) { max_brick_jobs_ = v; }
	int get_max_brick_jobs() const { return max_brick_jobs_; }
	void set_world_origin_bricks(Vector3i v) { world_origin_bricks_ = v; }
	Vector3i get_world_origin_bricks() const { return world_origin_bricks_; }
	void set_world_size_regions(Vector3i v) { world_size_regions_ = v; }
	Vector3i get_world_size_regions() const { return world_size_regions_; }
	void set_residency_radius_m(float v) { residency_radius_m_ = v; }
	float get_residency_radius_m() const { return residency_radius_m_; }

	GpuAtlas *atlas() { return atlas_; }
	ve::WorldBounds world_bounds() const;

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

	// Test/debug hooks for the GPU differential harness (spec §8). debug_eval_field takes
	// the SAME PackedByteArray the GPU op buffer is filled from, so the op struct layout is
	// verified end to end rather than transcribed twice.
	String debug_load_shader(const String &res_path) const;
	Vector2 debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const;

	// Debug/test hooks. Task 12 folds debug_init_atlas() into ensure_initialized().
	bool debug_init_atlas();
	void debug_teardown_atlas();
	Dictionary debug_atlas_stats();
	void debug_reset_frame_counters();
	void debug_set_region_map_entry(int region_index, int region_slot);
	void debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count);
	RID debug_mat_atlas() const;
	RID debug_mip_atlas(int level) const;
	RID debug_region_map() const;
	RID debug_region_tables() const;
	RID debug_free_list() const;
	RID debug_frame_counters() const;
	RID debug_op_pool() const;
	RID debug_op_counts() const;
};

} // namespace godot
