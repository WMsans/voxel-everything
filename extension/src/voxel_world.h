#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <atomic>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include "connectivity/occupancy.h"
#include "generator/volume_set.h"
#include "lod/lod_tree.h"
#include "mesh/chunk_residency.h"
#include "physics/island_body.h"
#include "physics/island_manager.h"
#include "render/island_atlas.h"
#include "world/edit_log.h"
#include "world/raycast.h"
#include "world/region.h"
#include "world/residency.h"

namespace godot {

class GpuAtlas;
class MaterialAtlas;
class RegionPass;
class BrickGenPass;
class RaymarchPass;
class CompositePass;
class WorldStreamer;
class MeshService;
class ColliderStreamer;
class LodPool;
class LodRasterPass;
class LodCullPass;
class HizPass;
class IslandAtlas;
class IslandCullPass;
struct IslandExtractJob;

// One edit drained by the streamer: the op plus the regions its append touched/rejected.
struct PendingEdit {
	ve::EditOp op;
	ve::EditLog::AppendResult result;
};

// One region's occupancy block on its way from the render thread to the main thread's grid.
struct OccupancyBlock {
	ve::IVec3 region{};
	int64_t seq = 0; // the world's edit sequence as of the mark that produced it
	std::vector<uint8_t> bytes; // ve::kOccupancyBlockBytes
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
	// Spec §6's "small bubbles around active bodies". Kept well under physics_radius_m_:
	// see ColliderStreamer::set_body_bubble_radius_m for why a body-sized bubble is not a
	// nicety but the difference between a linear and a quadratic collision plan.
	float physics_bubble_radius_m_ = 12.0f;
	int max_collider_chunks_ = 1280;
	int mesh_jobs_per_frame_ = 2;
	int shape_builds_per_frame_ = 2;

	GpuAtlas *atlas_ = nullptr;
	MaterialAtlas *materials_ = nullptr;
	IslandAtlas *islands_ = nullptr;
	int island_slots_ = 0; // high-water mark, not a population; guarded by island_mutex_
	IslandCullPass *island_cull_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *gen_pass_ = nullptr;
	RaymarchPass *raymarch_pass_ = nullptr;
	CompositePass *composite_pass_ = nullptr;
	LodRasterPass *lod_raster_pass_ = nullptr;
	LodCullPass *lod_cull_pass_ = nullptr;
	HizPass *hiz_pass_ = nullptr;
	// CPU cores outlive the GPU objects: a re-init re-streams the same world, edits
	// included. This is also what a future save/reload will do (saves ARE the edit log).
	ve::EditLog *edit_log_ = nullptr;
	// The authoritative copy of every stored volume. Owned here because it outlives the GPU
	// objects exactly as the edit log does: a re-init re-uploads the same rubble.
	ve::VolumeSet volumes_;
	ve::RegionResidency *residency_ = nullptr;
	WorldStreamer *streamer_ = nullptr;
	std::mutex edit_mutex_;                   // guards edit_log_ + pending_edits_
	std::vector<PendingEdit> pending_edits_;  // appended by tools, drained by the streamer
	int overflow_seen_ = 0;                   // sticky OR of frame overflow bits (tests)

	ve::OccupancyGrid occupancy_;              // main thread only
	std::mutex occupancy_mutex_;               // guards occupancy_inbox_
	std::vector<OccupancyBlock> occupancy_inbox_;
	// Monotonic; bumped by every accepted edit. The streamer stamps each occupancy readback
	// with it so IslandManager (Task 13) can tell whether a window's cells are new enough to
	// act on, rather than running connectivity against a picture of the world from before
	// the blast.
	std::atomic<int64_t> edit_seq_{0};
	void drain_occupancy();                    // inbox -> grid

	// The mesher runs on its own thread and owns its local RenderingDevice there; see
	// MeshService. Nothing on the main thread touches that device.
	MeshService *mesh_ = nullptr;
	ve::ChunkResidency *chunks_ = nullptr;
	ColliderStreamer *colliders_ = nullptr;
	IslandManager *island_manager_ = nullptr;
	mutable std::mutex island_mutex_; // also guards island_manager_ and island_slots_
	// Bytes on their way to a GPU pool. Filled on the main thread, drained on the render
	// thread by the compositor before it runs the streamer -- an op that names a volume must
	// never be evaluated before the volume is there.
	struct IslandUpload {
		int slot = -1;
		bool to_island_atlas = false; // false = the field volume pool
		ve::VolumeData data;
	};
	std::vector<IslandUpload> island_uploads_;
	std::vector<IslandSlotDesc> island_descs_;
	bool island_descs_dirty_ = false;
	std::vector<float> physics_bubble_centers_;
	std::atomic<int> debug_field_volume_upload_count_{0};
	bool physics_ready_ = false;
	std::vector<std::pair<ve::IVec3, ve::IVec3>> pending_dirty_; // guarded by edit_mutex_
	// A hand-driven body pool for tests. Task 13's IslandManager owns the real one and
	// takes these over; until then this is what proves the body path works.
	std::vector<IslandBody *> test_bodies_;
	float last_physics_tick_ms_ = 0.0f; // diagnostic; see debug_perf_stats

	// --- M5 LoD state (Task 12) ---
	int max_lod_pages_ = 32768;
	int lod_builds_per_frame_ = 8;
	// A chunk key is (level, coord); IVec3 has no operator< so the map key is explicit.
	struct LodKey {
		int level = 0;
		int x = 0;
		int y = 0;
		int z = 0;
		bool operator<(const LodKey &o) const {
			if (level != o.level) return level < o.level;
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	ve::LodTree *lod_tree_ = nullptr;
	LodPool *lod_pool_ = nullptr;
	uint32_t lod_frame_ = 0;
	ve::LodWalkResult lod_walk_;
	std::map<LodKey, std::vector<int>> lod_pages_of_;
	std::map<int, int> lod_page_quads_; // page -> number of quads stored in that page
	int lod_pressure_ = 0;
	void ensure_lod(); // lazy: creates/initializes lod_tree_ + lod_pool_ on first use

	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	bool initialized_ = false;

	void teardown_gpu(); // every GPU object; CPU cores survive
	// Gathers the ops that can affect a LoD chunk: its AABB padded by two cells, flattened
	// across regions in global append order, truncated to a chronological prefix (M4 errata 1).
	void gather_lod_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out);
	bool extract_component(const std::vector<ve::IVec3> &cells, IslandExtractJob *job,
			std::vector<ve::CellBox> *boxes, ve::VolumeData *out);

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
	void set_physics_bubble_radius_m(float v);
	float get_physics_bubble_radius_m() const { return physics_bubble_radius_m_; }
	void set_max_collider_chunks(int v) { max_collider_chunks_ = v; }
	int get_max_collider_chunks() const { return max_collider_chunks_; }
	void set_mesh_jobs_per_frame(int v) { mesh_jobs_per_frame_ = v; }
	int get_mesh_jobs_per_frame() const { return mesh_jobs_per_frame_; }
	void set_shape_builds_per_frame(int v) { shape_builds_per_frame_ = v; }
	int get_shape_builds_per_frame() const { return shape_builds_per_frame_; }
	void set_max_lod_pages(int v) { max_lod_pages_ = v; }
	int get_max_lod_pages() const { return max_lod_pages_; }
	void set_lod_builds_per_frame(int v) { lod_builds_per_frame_ = v; }
	int get_lod_builds_per_frame() const { return lod_builds_per_frame_; }
	void lod_tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ);
	// Push the current lod_walk_ page list (with per-page quad counts) into the raster pass.
	void prepare_lod_raster();
	RenderingDevice *rd() const;
	ve::WorldBounds world_bounds() const;

	GpuAtlas *atlas() { return atlas_; }
	MaterialAtlas *material_atlas() { return materials_; }
	IslandAtlas *islands() { return islands_; }
	// High-water mark, not a population: the shader masks off bits at or above it and then
	// tests each remaining slot's descriptor for dim >= 2, so a dead slot below the mark
	// costs one branch and nothing else. Non-inline: the render thread calls this and must
	// take island_mutex_ before touching island_manager_ / island_slots_.
	int island_slot_count() const;
	WorldStreamer *streamer() { return streamer_; }
	ve::EditLog *edit_log() { return edit_log_; }
	ve::VolumeSet &volumes() { return volumes_; }
	RaymarchPass *raymarch_pass() { return raymarch_pass_; }
	IslandCullPass *island_cull() { return island_cull_; }
	CompositePass *composite_pass() { return composite_pass_; }
	LodPool *lod_pool() { return lod_pool_; }
	LodRasterPass *lod_raster_pass() { return lod_raster_pass_; }
	LodCullPass *lod_cull_pass() { return lod_cull_pass_; }
	HizPass *hiz_pass() { return hiz_pass_; }
	std::mutex &edit_mutex() { return edit_mutex_; }
	MeshService *mesh_service() { return mesh_; }
	void queue_island_upload(int slot, const ve::VolumeData &d);
	void queue_field_volume_upload(int slot, const ve::VolumeData &d);
	// Removes a queued field-volume upload for `slot` (render handoff and worker pending
	// queue). Used when a re-merge paste is fully rejected before the uploads drain: the
	// slot is released (or restored to the body's birth volume), so its stale bytes must not
	// land in a later reused volume.
	void discard_field_volume_upload(int slot);
	void publish_island_descriptors(const std::vector<IslandSlotDesc> &d);
	void set_physics_bubbles(const std::vector<IslandBody *> &bodies);
	// A downward ve::raycast at (xz[0], xz[1]) from above the world, on the analytic field
	// plus its region ops and volumes -- the same call debug_raycast makes. The manager may
	// not build its own EditLog view, and this is the one field query it needs.
	ve::RayHit analytic_raycast_down(const float xz[2]);
	// Drained by RaymarchCompositor on the render thread; returns how many landed.
	int drain_island_uploads(RenderingDevice *device);

	int debug_island_frame(float dt, Vector3 center);
	Dictionary debug_island_stats();
	// Test hooks for teardown/reinit: let a test queue stale island GPU handoffs and observe
	// that teardown_physics() clears them before the next physics lifetime starts.
	int debug_island_pending_uploads();
	// Test hook: how many field-volume uploads have actually been handed to the GPU since
	// this VoxelWorld was created. A fully rejected re-merge paste must not increment this
	// even though queue_field_volume_upload ran before append_edit.
	int debug_field_volume_upload_count() const;
	int debug_island_descriptors_pending();
	// Test hook: slots the current MeshService has accepted through submit_volume since it
	// started. Verifies pinned volumes are replayed into a new worker after physics re-init.
	PackedInt32Array debug_mesh_volume_slots();
	void debug_queue_test_island_upload(int slot, const PackedByteArray &sdf,
			const PackedByteArray &mat, int dim);
	void debug_queue_test_island_descriptors();
	// Test hook: store, pin, and queue a field-volume upload the way a committed re-merge or
	// restore does. Teardown must preserve this upload across physics re-init because an edit
	// log op may already reference the pinned slot.
	void debug_queue_committed_field_volume_upload(int slot, const PackedByteArray &sdf,
			const PackedByteArray &mat, int dim);
	// Test hook: force the mesher's extraction availability flag so the engine suite can
	// simulate a permanently unavailable island extraction pass.
	void debug_set_extraction_available(bool v);
	// Test hook: force field extractions to fail even when the worker pass exists.
	void debug_set_fail_extractions(bool v);
	// Test hook: make the mesher reject extraction submits even when it is idle, so the
	// island manager's submit rollback path can be exercised deterministically.
	void debug_set_fail_extract_submit(bool v);
	void debug_set_merge_sleep_seconds(float v);
	// Test hook: lower the dynamic-body guardrail so a small test can prove slot-pool holes
	// after merges do not count against the cap.
	void debug_set_max_dynamic_bodies(int v);
	// Test hook: mark/unmark an island-atlas slot as used so a small test can fill the
	// 32-slot ceiling without spawning 32 real islands.
	void debug_set_atlas_slot_used(int slot, bool used);
	// Test hook: make the next island spawn fail before any carve so the no-carve fail-soft
	// path can be exercised without depending on a Jolt failure mode.
	void debug_set_fail_next_spawn(bool fail);
	// Test hook: make the next carve-rejection restore appear not to cover every carved
	// region, exercising the keep-the-body-alive path without depending on an op-cap race.
	void debug_set_fail_next_restore(bool fail);
	// Test hook: treat the next carve as rejected after at least one box has been accepted,
	// exercising the post-spawn carve-rejection path without depending on an op-cap race.
	void debug_set_fail_next_carve(bool fail);
	// Test hook: make the next re-merge resample fail so the resample backoff path can be
	// exercised without depending on a worker-side failure mode.
	void debug_set_fail_next_resample(bool fail);
	void debug_set_empty_next_extraction(bool v);
	// Test hook: wake an island body after a re-merge resample has been submitted, so the
	// stale-rest-pose guard can be exercised deterministically.
	void debug_wake_island_body(int index);
	// Test hook: offset and wake a live island body, for the stale-rest-pose regression.
	void debug_offset_island_body(int index, Vector3 offset);
	// Diagnostic: full physics-server state of a live island body plus a downward motion
	// query, for diagnosing islands that do not fall.
	Dictionary debug_island_body_info(int index);
	// Diagnostic: residency/build state of a collision chunk, for diagnosing stale colliders.
	Dictionary debug_chunk_collider_info(Vector3i chunk);

	// Tool entry point (VoxelEditTool, Task 14). Main thread; takes edit_mutex_.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
	// Low-level append used by IslandManager to hold edit_mutex_ across a carve/restore
	// sequence. The caller MUST already hold edit_mutex_().
	// `notify_islands` is false only for the island manager's own crumble carve: the matter
	// it removes was already labelled UNANCHORED, so nothing that was holding on can be
	// loosened by its going, and enqueueing a window would relabel the same neighbourhood
	// every time a speck of sub-voxel dust is swept up.
	ve::EditLog::AppendResult append_edit_locked(const ve::EditOp &op,
			bool notify_islands = true);

	// --- debug/test hooks (Tasks 7-10 kept; debug_sdf_atlas now returns the ATLAS) ---
	String debug_load_shader(const String &res_path) const;
	void debug_store_volume(int slot, const PackedByteArray &sdf, const PackedByteArray &mat,
			int dim);
	Vector2 debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count);
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

	// --- Task 8 hooks ---
	ve::OccupancyGrid &occupancy() { return occupancy_; }
	int64_t edit_seq() const { return edit_seq_.load(std::memory_order_relaxed); }
	int debug_occupancy_state(Vector3i cell);
	// The world field's signed distance at a point: generator + that point's region ops +
	// the volume store, the same evaluation ve::raycast marches. Diagnostic.
	float debug_field_sdf(Vector3 p);
	int debug_cell_state(Vector3i cell);
	Dictionary debug_occupancy_stats(Vector3 center);

	// --- Task 12 hooks ---
	Color debug_raymarch_pixel(Vector3 origin, Vector3 dir);
	Dictionary debug_raymarch_probe(Vector3 origin, Vector3 dir);
	// --- M5 Task 11 hooks ---
	Dictionary debug_material_atlas_stats();
	Color debug_material_probe(int mat, Vector3 p, Vector3 n);
	int debug_stream_frame(Vector3 cam);
	Dictionary debug_stream_stats();
	int debug_slot_of_region(Vector3i region) const;
	int debug_region_map_entry(Vector3i region);
	bool debug_region_map_consistent();
	Dictionary debug_raycast(Vector3 origin, Vector3 dir);
	RenderingDevice *debug_local_rd() const { return local_rd_; }

	// --- Task 12 body hooks ---
	Dictionary debug_spawn_test_body(Vector3i lo_cell, Vector3i hi_cell, Vector3 offset,
			Vector3 impulse, bool debris);
	Dictionary debug_test_body_stats(int index);
	void debug_tick_test_bodies(float dt);
	void debug_despawn_test_body(int index);

	// --- Task 4 hooks ---
	bool debug_init_physics();
	void debug_teardown_physics();
	Dictionary debug_mesh_lattice_diff(Vector3i chunk);
	int debug_physics_frame(Vector3 center);
	// Test hook: stand in for the island manager's live bodies, so the bubble policy can be
	// exercised without spawning (and waiting on) real islands.
	void debug_set_physics_bubbles(const PackedVector3Array &centers);
	Dictionary debug_physics_stats();
	// Per-phase frame timings for the two streaming paths. Diagnostic only: the HUD and the
	// benchmark read it to say WHERE a frame went, rather than that it was slow.
	Dictionary debug_perf_stats();
	RID debug_body_of_chunk(Vector3i chunk);

	// --- Task 5 hook ---
	Dictionary debug_mesh_diff(Vector3i chunk);

	// --- M5 Task 9 hooks ---
	Dictionary debug_lod_diff(int level, Vector3i coord);
	void debug_apply_sphere_subtract(Vector3 centre, float radius);

	// --- Task 9 hook ---
	Dictionary debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell);

	// --- Task 10 hooks ---
	Dictionary debug_place_test_island(int slot, Vector3i lo_cell, Vector3i hi_cell,
			Vector3 offset);
	Dictionary debug_place_test_island_rotated(int slot, Vector3i lo_cell, Vector3i hi_cell,
			Vector3 offset, float yaw);
	void debug_clear_test_island(int slot);
	PackedInt32Array debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
			float tan_y, int width, int height);

	// --- Task 6 hooks ---
	bool debug_mesh_submit(Array chunks);
	Array debug_mesh_collect();

	// --- M5 Task 10 LoD queue hooks ---
	bool debug_lod_submit(Array jobs);
	Array debug_lod_collect();
	// --- M5 Task 12 LoD tick hooks ---
	void debug_lod_tick(Vector3 pos, Vector3 fwd);
	Dictionary debug_lod_stats();
	// --- M5 Task 13 LoD render hooks ---
	Dictionary debug_lod_render_probe(Vector3 pos, Vector3 fwd, int w, int h);
	Dictionary debug_lod_render_probe_culled(Vector3 pos, Vector3 fwd, int w, int h,
			bool cull);
	// --- M5 Task 14 HiZ hooks ---
	Dictionary debug_hiz_stats();
	Dictionary debug_hiz_probe_synthetic(float far_value, float near_value);
	bool debug_hiz_occluded(Vector2 lo, Vector2 hi, float depth);
	// --- M5 Task 15 LoD cull hooks ---
	Dictionary debug_lod_cull_probe(Vector3 pos, Vector3 fwd);
};

} // namespace godot
