#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <mutex>
#include <utility>
#include <vector>
#include "mesh/chunk_residency.h"
#include "world/edit_log.h"
#include "world/region.h"
#include "world/residency.h"

namespace godot {

class GpuAtlas;
class RegionPass;
class BrickGenPass;
class RaymarchPass;
class CompositePass;
class WorldStreamer;
class MeshPass;
class ColliderStreamer;

// One edit drained by the streamer: the op plus the regions its append touched/rejected.
struct PendingEdit {
	ve::EditOp op;
	ve::EditLog::AppendResult result;
};

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

	bool use_local_device_ = false;

	Vector3i atlas_bricks_ = Vector3i(64, 32, 32);
	int max_region_slots_ = 512;
	int max_brick_jobs_ = 16384;
	Vector3i world_origin_bricks_ = Vector3i(0, -64, 0);
	Vector3i world_size_regions_ = Vector3i(64, 8, 64);
	float residency_radius_m_ = 96.0f;

	bool physics_enabled_ = true;
	NodePath physics_center_path_;
	float physics_radius_m_ = 64.0f;
	int max_collider_chunks_ = 160;
	int mesh_jobs_per_frame_ = 2;
	int shape_builds_per_frame_ = 2;

	GpuAtlas *atlas_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *gen_pass_ = nullptr;
	RaymarchPass *raymarch_pass_ = nullptr;
	CompositePass *composite_pass_ = nullptr;
	// CPU cores outlive the GPU objects: a re-init re-streams the same world, edits
	// included. This is also what a future save/reload will do (saves ARE the edit log).
	ve::EditLog *edit_log_ = nullptr;
	ve::RegionResidency *residency_ = nullptr;
	WorldStreamer *streamer_ = nullptr;
	std::mutex edit_mutex_;                   // guards edit_log_ + pending_edits_
	std::vector<PendingEdit> pending_edits_;  // appended by tools, drained by the streamer
	int overflow_seen_ = 0;                   // sticky OR of frame overflow bits (tests)

	RenderingDevice *mesh_rd_ = nullptr; // owned; ALWAYS local (submit/sync are illegal on main)
	MeshPass *mesh_pass_ = nullptr;
	ve::ChunkResidency *chunks_ = nullptr;
	ColliderStreamer *colliders_ = nullptr;
	bool physics_ready_ = false;
	std::vector<std::pair<ve::IVec3, ve::IVec3>> pending_dirty_; // guarded by edit_mutex_

	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	bool initialized_ = false;

	void teardown_gpu(); // every GPU object; CPU cores survive

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;
	~VoxelWorld() override;

	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
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

	void ensure_initialized();
	bool is_initialized() const { return initialized_; }
	void ensure_physics_initialized();
	void teardown_physics();
	int physics_tick(Vector3 center); // returns actions taken; Task 7 gives it a body
	bool is_physics_ready() const { return physics_ready_; }
	void set_physics_enabled(bool v) { physics_enabled_ = v; }
	bool get_physics_enabled() const { return physics_enabled_; }
	void set_physics_center_path(const NodePath &p) { physics_center_path_ = p; }
	NodePath get_physics_center_path() const { return physics_center_path_; }
	void set_physics_radius_m(float v) { physics_radius_m_ = v; }
	float get_physics_radius_m() const { return physics_radius_m_; }
	void set_max_collider_chunks(int v) { max_collider_chunks_ = v; }
	int get_max_collider_chunks() const { return max_collider_chunks_; }
	void set_mesh_jobs_per_frame(int v) { mesh_jobs_per_frame_ = v; }
	int get_mesh_jobs_per_frame() const { return mesh_jobs_per_frame_; }
	void set_shape_builds_per_frame(int v) { shape_builds_per_frame_ = v; }
	int get_shape_builds_per_frame() const { return shape_builds_per_frame_; }
	RenderingDevice *rd() const;
	ve::WorldBounds world_bounds() const;

	GpuAtlas *atlas() { return atlas_; }
	WorldStreamer *streamer() { return streamer_; }
	ve::EditLog *edit_log() { return edit_log_; }
	RaymarchPass *raymarch_pass() { return raymarch_pass_; }
	CompositePass *composite_pass() { return composite_pass_; }
	std::mutex &edit_mutex() { return edit_mutex_; }

	// Tool entry point (VoxelEditTool, Task 14). Main thread; takes edit_mutex_.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);

	// --- debug/test hooks (Tasks 7-10 kept; debug_sdf_atlas now returns the ATLAS) ---
	String debug_load_shader(const String &res_path) const;
	Vector2 debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const;
	bool debug_init_atlas();
	void debug_teardown_atlas();
	Dictionary debug_atlas_stats();
	void debug_reset_frame_counters();
	void debug_set_region_map_entry(int region_index, int region_slot);
	void debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count);
	bool debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops, int op_count) const;
	void debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
			int op_count, bool force);
	void debug_release_region(int region_slot);
	PackedInt32Array debug_jobs();
	int debug_region_table_slot(int region_slot, Vector3i brick);
	void debug_generate_pending();
	Dictionary debug_brick_diff(Vector3i brick, int region_slot, const PackedByteArray &ops,
			int op_count);
	RID debug_sdf_atlas() const;
	RID debug_mat_atlas() const;
	RID debug_mip_atlas(int level) const;
	RID debug_region_map() const;
	RID debug_region_tables() const;
	RID debug_free_list() const;
	RID debug_frame_counters() const;
	RID debug_op_pool() const;
	RID debug_op_counts() const;

	// --- Task 12 hooks ---
	Color debug_raymarch_pixel(Vector3 origin, Vector3 dir);
	Dictionary debug_raymarch_probe(Vector3 origin, Vector3 dir);
	int debug_stream_frame(Vector3 cam);
	Dictionary debug_stream_stats();
	int debug_slot_of_region(Vector3i region) const;
	int debug_region_map_entry(Vector3i region);
	bool debug_region_map_consistent();
	Dictionary debug_raycast(Vector3 origin, Vector3 dir);
	RenderingDevice *debug_local_rd() const { return local_rd_; }

	// --- Task 4 hooks ---
	bool debug_init_physics();
	void debug_teardown_physics();
	Dictionary debug_mesh_lattice_diff(Vector3i chunk);
};

} // namespace godot
