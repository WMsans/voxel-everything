#pragma once
// VoxelWorld -- the scene node, and a façade: ClassDB properties and methods, the CPU-side init
// order (graphics, then physics), and ownership of the modules that do the work --
//   WorldStore                authoritative CPU data (config, edit log, overrides, volumes)
//   LodSystem                 far-field runtime
//   RenderOrchestrator        render lifetime: devices, passes, streamer, island handoff, frame
//   ConsolidationCoordinator  override consolidation
// C++ callers reach those modules through context(); this class forwards only what ClassDB
// binds. Spec: docs/superpowers/specs/2026-09-14-render-lifetime-owner-design.md.
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include "core/context.h"
#include "core/world_store.h"
#include "debug/hooks.h"
#include "mesh/box_merge.h"
#include "mesh/chunk_residency.h"
#include "world/edit_log.h"
#include "world/region.h"

namespace godot {

class ColliderStreamer;
class ConsolidationCoordinator;
class IslandBody;
class IslandManager;
class LodSystem;
class MeshService;
class RenderOrchestrator;
class VoxelWorld;
struct IslandExtractJob;

bool voxel_compositor_callbacks_enabled();
bool voxel_try_begin_compositor_callback(const NodePath &world_path, VoxelWorld **world);

// Counters the debug facade reports; copied out, never referenced.
struct WorldStats {
	int overflow_seen = 0;   // sticky OR of streamer overflow bits
	int edit_rejections = 0; // ops refused by a full region list
	float last_physics_tick_ms = 0.0f;
};

class VoxelWorld : public Node3D, public ve::InvalidationSink {
	GDCLASS(VoxelWorld, Node3D)

	VoxelDebugHooks *debug_hooks_ = nullptr;
	bool use_local_device_ = false;
	std::unique_ptr<WorldStore> store_; // created first: setters write its config pre-init
	VoxelContext context_;
	std::unique_ptr<ConsolidationCoordinator> consolidation_;
	// Declared before render_: the orchestrator owns the frame, which references this LoD
	// runtime, so the LoD runtime must be destroyed after it.
	std::unique_ptr<LodSystem> lod_;
	std::unique_ptr<RenderOrchestrator> render_;

	bool physics_enabled_ = true;
	NodePath physics_center_path_;
	NodePath sun_light_path_;
	float physics_radius_m_ = 64.0f;
	// Kept well under physics_radius_m_: see ColliderStreamer::set_body_bubble_radius_m.
	float physics_bubble_radius_m_ = 12.0f;
	int max_collider_chunks_ = 1280;
	int mesh_jobs_per_frame_ = 2;
	int shape_builds_per_frame_ = 2;
	// The golden corpora pin their own frozen pipeline through this.
	String terrain_pipeline_path_ = "res://assets/pipelines/default.pipeline";

	// Physics lifetime (ensure_physics_initialized / teardown_physics). The mesher owns its own
	// device on its own thread; nothing on the main thread touches that device.
	MeshService *mesh_ = nullptr;
	ve::ChunkResidency *chunks_ = nullptr;
	ColliderStreamer *colliders_ = nullptr;
	IslandManager *island_manager_ = nullptr; // published/detached under edit_mutex
	bool physics_ready_ = false;
	// Collider remesh queue; guarded by edit_mutex, drained by physics_tick. Bounded by
	// ve::merge_or_cap (core/edit_pipeline.h): overlapping ranges merge, so a thousand edits
	// in one place stay one entry.
	std::vector<ve::Box3<int>> pending_dirty_;
	std::vector<float> physics_bubble_centers_;                  // xyz triples, main thread
	std::vector<IslandBody *> test_bodies_;                      // hand-driven test pool
	WorldStats stats_;

	void update_sun_state();
	void publish_sun_state_to_local_device(RenderingDevice *device);
	// InvalidationSink: the collider remesh queue (kEdit) and the rejection stats
	// (kRejected). Edit lock held; queue only (core/edit_pipeline.h).
	void record(const ve::Invalidation &inv) override;

protected:
	static void _bind_methods();

public:
	VoxelWorld();
	~VoxelWorld() override;
	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;

	VoxelDebugHooks *hooks();
	// The material registry for the demo's picker; one dictionary per material, id ascending.
	Array material_table() const;
	VoxelContext &context() { return context_; }

	// --- ClassDB properties ---
	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	void set_atlas_bricks(Vector3i v);
	Vector3i get_atlas_bricks() const;
	void set_max_region_slots(int v);
	int get_max_region_slots() const;
	void set_max_brick_jobs(int v);
	int get_max_brick_jobs() const;
	void set_max_override_bricks(int v);
	int get_max_override_bricks() const;
	void set_stream_radius_m(float v);
	float get_stream_radius_m() const;
	void set_occupancy_retention_m(float v);
	float get_occupancy_retention_m() const;
	void set_residency_radius_m(float v);
	float get_residency_radius_m() const;
	// Fraction of the internal resolution the near-field marcher runs at: the frame budget's
	// coarsest dial. Stored on the orchestrator (read on the render thread).
	void set_near_field_scale(float v);
	float get_near_field_scale() const;
	void set_physics_enabled(bool v) { physics_enabled_ = v; }
	bool get_physics_enabled() const { return physics_enabled_; }
	void set_physics_center_path(const NodePath &p) { physics_center_path_ = p; }
	NodePath get_physics_center_path() const { return physics_center_path_; }
	void set_sun_light_path(const NodePath &p) { sun_light_path_ = p; }
	NodePath get_sun_light_path() const { return sun_light_path_; }
	// The A/B knob for the shadow cut's minimum-level clamp. On by default.
	void set_sun_cascade_min_level(bool v);
	bool get_sun_cascade_min_level() const;
	void set_physics_radius_m(float v) { physics_radius_m_ = v; }
	float get_physics_radius_m() const { return physics_radius_m_; }
	void set_physics_bubble_radius_m(float v); // applies live to the collider streamer
	float get_physics_bubble_radius_m() const { return physics_bubble_radius_m_; }
	void set_max_collider_chunks(int v) { max_collider_chunks_ = v; }
	int get_max_collider_chunks() const { return max_collider_chunks_; }
	void set_mesh_jobs_per_frame(int v) { mesh_jobs_per_frame_ = v; }
	int get_mesh_jobs_per_frame() const { return mesh_jobs_per_frame_; }
	void set_shape_builds_per_frame(int v) { shape_builds_per_frame_ = v; }
	int get_shape_builds_per_frame() const { return shape_builds_per_frame_; }
	void set_max_lod_pages(int v);
	int get_max_lod_pages() const;
	void set_max_lod_chunk_records(int v);
	int get_max_lod_chunk_records() const;
	void set_lod_builds_per_frame(int v);
	int get_lod_builds_per_frame() const;
	void set_terrain_pipeline_path(const String &v) { terrain_pipeline_path_ = v; }
	String get_terrain_pipeline_path() const { return terrain_pipeline_path_; }
	void set_quality_tier(int v);
	int get_quality_tier() const;
	// Fail-soft: an unknown effect name is ignored, not a crash.
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	void set_effect_value(const String &name, float value);
	float get_effect_value(const String &name) const;
	bool set_grass_value(const String &name, float v);
	float get_grass_value(const String &name) const;

	// --- lifetime ---
	void ensure_initialized();
	bool is_initialized() const;
	bool load_terrain_pipeline(); // first successful load wins; see the definition
	void request_shader_reload(); // a latch; the render callback pumps it
	void teardown_gpu();          // every GPU object; CPU cores survive
	void shutdown_render_resources();
	// ClassDB-bound as "_shutdown_render_resources_on_render_thread" (the Callable target).
	void shutdown_render_resources_on_render_thread();
	// Compositor admission guard; _exit_tree waits for admitted callbacks.
	bool try_begin_render_callback();
	void end_render_callback();
	// On a local device this also refreshes and publishes the sun (see the definition).
	RenderingDevice *rd() const;
	int sun_cascade_count() const;

	// --- physics ---
	void ensure_physics_initialized();
	void teardown_physics();
	int physics_tick(Vector3 center); // actions taken
	MeshService *mesh_service() { return mesh_; }
	ColliderStreamer *colliders() { return colliders_; }
	ve::ChunkResidency *chunk_residency() { return chunks_; }
	IslandManager *island_manager() { return island_manager_; }
	bool physics_ready() const { return physics_ready_; }
	std::vector<IslandBody *> &test_bodies() { return test_bodies_; }
	std::vector<float> &physics_bubble_centers() { return physics_bubble_centers_; }
	// The collider remesh queue, for debug_edit_fanout. Guarded by edit_mutex: the caller
	// must hold it.
	const std::vector<ve::Box3<int>> &pending_dirty() const { return pending_dirty_; }
	WorldStats stats() const { return stats_; }
	void note_overflow(int bits) { stats_.overflow_seen |= bits; }
	// Synchronous island extraction for diagnostics (drives the mesher worker by hand).
	bool extract_component(const std::vector<ve::IVec3> &cells, IslandExtractJob *job,
			std::vector<ve::CellBox> *boxes, ve::VolumeData *out);

	// --- edits ---
	// Tool entry point; main thread; takes edit_mutex.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
	// GDScript "append_edit": one op in its 32-byte ve::EditOp encoding -> {touched, rejected}.
	Dictionary append_edit_op(const PackedByteArray &op_bytes);
	// GDScript "raycast": the CPU field ray gameplay aims with -> {hit, pos, normal,
	// distance, material}. `material` is the struck solid's, for hardness-aware tools.
	Dictionary raycast(Vector3 origin, Vector3 dir, float max_distance = 200.0f);
	int64_t edit_seq() const { return store_->edit_seq(); }
};

} // namespace godot
