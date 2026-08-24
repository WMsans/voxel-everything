#pragma once
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
// VoxelDebugHooks — all debug_*/test-fixture entry points extracted from
// VoxelWorld (spec Phase 1). Registered as a Godot class so GDScript tests
// call world.hooks().debug_x(...). Holds no state of its own beyond a
// back-reference set by its owning VoxelWorld.
namespace godot {
class VoxelWorld;
class VoxelDebugHooks : public Object {
	GDCLASS(VoxelDebugHooks, Object)
public:
	void bind_world(VoxelWorld *w) { world_ = w; }
	Dictionary debug_shader_reload_stats();

	void debug_pump_shader_reload();

	void debug_set_shader_override(const String &name, const String &source);

	// Differential self-check: runs the CPU-vs-GPU diff machinery the gdUnit suites use and
	// returns a single dictionary a running demo can print.
	Dictionary debug_self_check();

	Dictionary debug_beauty_settings();

	Dictionary debug_beauty_compositor_stats();

	Dictionary debug_gpu_timings();

	Dictionary debug_ingest_gpu_timings(const PackedStringArray &, const PackedInt64Array &, int64_t);

	Dictionary debug_contact_shadow_probe(Vector3 pos, Vector3 fwd, int w, int h);

	Dictionary debug_ssr_probe(int fixture, int w, int h);

	Dictionary debug_outline_probe(int fixture, bool have_dynamic_normals);

	Dictionary debug_glossy_sdf_probe(Vector3 origin, Vector3 dir);

	Dictionary debug_ssgi_probe(Vector3 pos, Vector3 fwd, int w, int h, int frames);

	Dictionary debug_ssao_probe(Vector3 pos, Vector3 fwd, int w, int h);

	Dictionary debug_ssgi_reprojection_probe(Vector3 previous_pos, Vector3 previous_fwd,
			Vector3 current_pos, Vector3 current_fwd, int w, int h);

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

	// --- Task 6 hooks: fixed-capacity stored-normal pool ---
	// Debug initializer: shrink the normal-pool budget BEFORE debug_init_atlas(). The
	// pool's size is otherwise fixed at exactly 32 MiB and never resizes.
	void debug_set_normal_pool_budget(int bytes);

	Dictionary debug_stored_normal_stats();

	// Task 8 teardown telemetry: RID validity plus the CPU mirror of the two offset
	// tables (an entry is non-minus-one exactly while a live span is published for that
	// slot, so "all_minus_one" is exactly "no spans published"). After render teardown
	// all three RIDs must be invalid; after reinitialization both tables must be -1.
	Dictionary debug_normal_pool_state();

	// Upload one override brick's packed uint16 normals; returns its published byte offset
	// into normal_buffer(), or -1 when the source entered the wide-R8 fallback.
	int64_t debug_normal_upload_override(int slot, const PackedByteArray &packed_normals);

	void debug_normal_release_override(int slot);

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

	void debug_set_fail_consolidations(bool v);

	void debug_set_fail_consolidate_uploads(bool v);

	void debug_set_fail_restore_overrides(bool v);

	void debug_set_fail_restore_overrides_always(bool v);

	void debug_set_pause_override_publication(bool v);

	bool debug_override_publication_paused() const;

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

	// --- debug/test hooks (Tasks 7-10 kept; debug_sdf_atlas now returns the ATLAS) ---
	String debug_load_shader(const String &res_path) const;

	void debug_store_volume(int slot, const PackedByteArray &sdf, const PackedByteArray &mat,
			int dim);

	Vector2 debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count);

	Dictionary debug_eval_field_gradient(Vector3 p, const PackedByteArray &ops, int op_count);

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

	void debug_stream_region(Vector3i region);

	Dictionary debug_brick_flags(Vector3i region);

	Dictionary debug_brick_flags_after_mark(Vector3i region);

	RID debug_sdf_atlas() const;

	RID debug_mat_atlas() const;

	RID debug_mip_atlas(int level) const;

	RID debug_region_map() const;

	RID debug_region_tables() const;

	RID debug_free_list() const;

	RID debug_frame_counters() const;

	RID debug_op_pool() const;

	RID debug_op_counts() const;

	int debug_occupancy_state(Vector3i cell);

	// Harvest already-issued occupancy readbacks; does not advance streaming or issue a mark.
	void debug_pump_occupancy();

	Dictionary debug_occupancy_diff(Vector3i region);

	Dictionary debug_occupancy_fallback_diff(Vector3i region);

	// The world field's signed distance at a point: generator + that point's region ops +
	// the volume store, the same evaluation ve::raycast marches. Diagnostic.
	float debug_field_sdf(Vector3 p);

	int debug_cell_state(Vector3i cell);

	Dictionary debug_occupancy_stats(Vector3 center);

	// --- Task 12 hooks ---
	Color debug_raymarch_pixel(Vector3 origin, Vector3 dir);

	Dictionary debug_raymarch_probe(Vector3 origin, Vector3 dir);

	Dictionary debug_raymarch_cost_probe(Vector3 origin, Vector3 dir);

	Dictionary debug_raymarch_gbuffer(Vector3 origin, Vector3 dir);

	Dictionary debug_raymarch_hole_probe(Vector3 origin, Vector3 dir, int w, int h);

	Dictionary debug_raymarch_normal_probe(Vector3 origin, Vector3 dir, int w, int h);

	// Task 7: same five metrics as the terrain probe, but the reference normal for every
	// hit inside the island's local lattice comes from that body's own VolumeData::normal_oct
	// (trilinearly blended in the body frame, then rotated by the body basis).
	Dictionary debug_island_normal_probe(int island_slot, Vector3 origin, Vector3 dir,
			int w, int h);

	// --- M5 Task 11 hooks ---
	Dictionary debug_material_atlas_stats();

	Color debug_material_probe(int mat, Vector3 p, Vector3 n);

	// Task 7: rewrites one material layer's normal-map texels (surface array RG) so tests
	// can prove the G-buffer normal never depends on the material normal map.
	bool debug_poke_material_normal(int layer);

	int debug_stream_frame(Vector3 cam);

	Dictionary debug_stream_stats();

	int debug_slot_of_region(Vector3i region) const;

	int debug_region_map_entry(Vector3i region);

	bool debug_region_map_consistent();

	Dictionary debug_raycast(Vector3 origin, Vector3 dir);

	RenderingDevice *debug_local_rd() const;

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

	Dictionary debug_chunk_collider_octants(Vector3i chunk);

	// --- Task 5 hook ---
	Dictionary debug_mesh_diff(Vector3i chunk);

	Dictionary debug_consolidate_diff(Vector3i region);

	bool debug_consolidate_region(Vector3i region);

	void debug_pump_consolidation();

	void debug_pump_consolidation_async();

	void debug_wait_consolidation();

	int debug_region_op_count(Vector3i region);

	int debug_override_used() const;

	// Test-only fixture: publish one valid table containing every override slot, exhausting
	// the real 8192-slot store so refusal tests do not rely on an oversized plan shortcut.
	bool debug_fill_override_pool();

	Dictionary debug_override_render_state(Vector3i brick);

	int debug_override_region_table(int region_slot) const;

	// --- M5 Task 9 hooks ---
	Dictionary debug_lod_diff(int level, Vector3i coord);

	void debug_apply_sphere_subtract(Vector3 centre, float radius);

	// Task 7 fixture hook: a sphere-ADD op so the artifact tests can exercise the
	// procedural CSG-add branch of the source-field normal path.
	void debug_apply_sphere_add(Vector3 centre, float radius, int material);

	void debug_apply_volume_add(int slot, Vector3 origin, float voxel, int dim);

	// --- Task 9 hook ---
	Dictionary debug_island_extract_diff(Vector3i lo_cell, Vector3i hi_cell);

	// --- Task 10 hooks ---
	Dictionary debug_place_test_island(int slot, Vector3i lo_cell, Vector3i hi_cell,
			Vector3 offset);

	Dictionary debug_place_test_island_rotated(int slot, Vector3i lo_cell, Vector3i hi_cell,
			Vector3 offset, float yaw, int volume_slot = -1);

	void debug_clear_test_island(int slot);

	PackedInt32Array debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
			float tan_y, int width, int height);

	// --- Task 6 hooks ---
	Dictionary debug_cel_diff(Color albedo, Color ambient, float ndl, float ndv, float ndh,
			float shadow, float ao, float gloss);

	Color debug_cel_reference(Color albedo, Color ambient, float ndl, float ndv, float ndh,
			float shadow, float ao, float gloss) const;

	Dictionary debug_deferred_probe(Vector3 pos, Vector3 fwd, int w, int h, int probe_mode);

	bool debug_mesh_submit(Array chunks);

	Array debug_mesh_collect();

	bool debug_extract_submit(int id, Vector3i lo_cell, Vector3i hi_cell);

	Array debug_extract_collect();

	// --- M5 Task 10 LoD queue hooks ---
	bool debug_lod_submit(Array jobs);

	Array debug_lod_collect();

	// --- M5 Task 12 LoD tick hooks ---
	void debug_lod_tick(Vector3 pos, Vector3 fwd);

	Dictionary debug_lod_stats();

	// x = fade start, y = fade end: the seam this frame, for tests that must not bake in a
	// distance the near field may not be able to pay for.
	Vector2 debug_lod_fade_band();

	// --- M5 Task 13 LoD render hooks ---
	Dictionary debug_lod_render_probe(Vector3 pos, Vector3 fwd, int w, int h);

	Dictionary debug_lod_render_probe_culled(Vector3 pos, Vector3 fwd, int w, int h,
			bool cull);

	Dictionary debug_lod_gbuffer_probe(Vector3 pos, Vector3 fwd, int w, int h);

	// --- M5 Task 16 seam hooks ---
	Dictionary debug_seam_probe(Vector3 pos, Vector3 fwd, int w, int h, bool skip_lod = false);

	// --- M5 Task 14 HiZ hooks ---
	Dictionary debug_hiz_stats();

	Dictionary debug_hiz_shutdown_probe();

	Dictionary debug_gbuffer_stats(int w, int h);

	Dictionary debug_hiz_probe_synthetic(float far_value, float near_value);

	bool debug_hiz_occluded(Vector2 lo, Vector2 hi, float depth);

	// --- M5 Task 15 LoD cull hooks ---
	Dictionary debug_lod_cull_probe(Vector3 pos, Vector3 fwd);

	// --- Task 8 hooks ---
	Dictionary debug_sun_shadow_stats();

	void debug_sun_shadow_build(bool force);

	float debug_sun_shadow_visibility(Vector3 p);
protected:
	static void _bind_methods();

private:
	VoxelWorld *world_ = nullptr;
};
} // namespace godot
