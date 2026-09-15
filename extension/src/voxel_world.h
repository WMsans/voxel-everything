#pragma once
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <algorithm>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <utility>
#include <vector>
#include <tuple>
#include "connectivity/occupancy.h"
#include "core/context.h"
#include "core/world_store.h"
#include "debug/hooks.h"
#include "generator/volume_set.h"
#include "grass/grass_settings.h"
#include "lod/lod_system.h"
#include "mesh/chunk_residency.h"
#include "physics/island_body.h"
#include "physics/island_manager.h"
#include "render/island_atlas.h"
#include "render/gpu_timings.h"
#include "render/orchestrator.h" // inline pass-graph delegations need the complete type
#include "render/frame.h"
#include "render/consolidate_pass.h"
#include "shade/beauty_settings.h"
#include "shade/sun_ortho.h"
#include "shade/sun_state.h"
#include "world/edit_log.h"
#include "world/region.h"
#include "world/override_store.h"
#include "world/residency.h"
#include "world/field_source_snapshot.h"

namespace godot {

// Compositor callbacks can outlive the SceneTree during SceneTree::quit(). Admission
// serializes the enabled check, SceneTree/world lookup, and per-world callback guard.
// The free admission functions are declared in render/orchestrator.h (Task 13): the
// per-world half of the admission state lives on RenderOrchestrator now.
class VoxelWorld;
bool voxel_compositor_callbacks_enabled();
bool voxel_try_begin_compositor_callback(const NodePath &world_path, VoxelWorld **world);

class GpuAtlas;
class ConsolidationCoordinator;
class MaterialAtlas;
class RegionPass;
class BrickGenPass;
class FieldContextSet;
class RaymarchPass;
class CompositePass;
class DeferredPass;
class SunShadowPass;
class SunUbo;
class InjectPass;
class WorldStreamer;
class MeshService;
class ColliderStreamer;
class LodPool;
class LodRasterPass;
class LodCullPass;
class HizPass;
class GBuffer;
class CameraUbo;
class ContactShadowPass;
class SsgiPass;
class SsaoPass;
class SsrPass;
class OutlinePass;
class GrassScatterPass;
class GrassRasterPass;
class IslandAtlas;
class IslandCullPass;
struct IslandExtractJob;

class VoxelWorld : public Node3D, public EditSink {
	GDCLASS(VoxelWorld, Node3D)
	// Strangler adapter: VoxelWorld satisfies WorldStore's notification ports and forwards
	// to the fan-out logic. The EditSink half is permanent by ruling -- IslandManager keeps
	// its own notification path, so VoxelWorld remains the EditSink; the ConsolidationSink
	// half died in Task 11, when ConsolidationCoordinator took over (it satisfies the port
	// directly).
	//
	// Last remaining friend (Task 13 removed the compositor/admission ones): the debug
	// facade pokes ~20 private members directly (store_, mesh_, colliders_, chunks_,
	// island_manager_, physics_ready_, test_bodies_, island uploads/desc
	// state, ...) plus 2 private helpers (render_probe_pixel, extract_component) -- audited
	// at Task 16. JUSTIFICATION: every one
	// of those accesses is live in debug/hooks.cpp; replacing the friendship would need
	// either an unbounded public accessor dump on this class or a wholesale rework of the
	// facade's world_ back-reference. Both are behavior-surface changes outside this
	// refactor's no-behavior-change guard (spec §8), so the friendship stays until that
	// dedicated rework. See task-16-report's friend table.
	friend class VoxelDebugHooks;

	VoxelDebugHooks *debug_hooks_ = nullptr;

	bool use_local_device_ = false;

	// Authoritative CPU data plane (Phase 2a): config + edit log / override /
	// volume / residency state live in WorldStore. Created FIRST, in the
	// constructor, so the property setters can write the config pre-init
	// exactly as they wrote the plain fields before the split.
	std::unique_ptr<WorldStore> store_;
	VoxelContext context_; // subsystem wiring; store_ is published here at construction
	// Owns the consolidation state machine (Task 11): queue/pump/publish/rollback plus all
	// consolidation_* members live there now; it satisfies WorldStore's ConsolidationSink
	// port directly. Handles-only collaborators (addresses of the fields below).
	std::unique_ptr<ConsolidationCoordinator> consolidation_;
	// Owns the LoD runtime (Task 15): THE lod mutex, tree/walk/page-map/pool state plus tick/
	// fade-band/op-gathering live in LodSystem now. Declared before render_: the orchestrator
	// owns the frame, which references this LoD runtime, so the LoD runtime must be destroyed
	// after it.
	std::unique_ptr<LodSystem> lod_;
	// GPU pass graph + device ownership (Task 12): every pass pointer, the downsample
	// pipeline and main_rd_/local_rd_ live in RenderOrchestrator now; C++ callers use
	// context() to reach the owning subsystem directly.
	std::unique_ptr<RenderOrchestrator> render_;

	bool physics_enabled_ = true;
	NodePath physics_center_path_;
	NodePath sun_light_path_;
	float physics_radius_m_ = 64.0f;
	// Spec §6's "small bubbles around active bodies". Kept well under physics_radius_m_:
	// see ColliderStreamer::set_body_bubble_radius_m for why a body-sized bubble is not a
	// nicety but the difference between a linear and a quadratic collision plan.
	float physics_bubble_radius_m_ = 12.0f;
	int max_collider_chunks_ = 1280;
	int mesh_jobs_per_frame_ = 2;
	int shape_builds_per_frame_ = 2;
	int overflow_seen_ = 0;                   // sticky OR of frame overflow bits (tests)
	int edit_rejections_ = 0; // append fan-out rejection stat; read by debug_stream_stats
	// The golden corpora pin their own frozen pipeline through this, so demo terrain can
	// change without invalidating the proof that the generator did not move.
	String terrain_pipeline_path_ = "res://assets/pipelines/default.pipeline";

	void update_sun_state();
	void publish_sun_state_to_local_device(RenderingDevice *device);
	// EditSink port satisfied for WorldStore's spine; adapter body forwards to today's
	// island-manager notification.
	void on_edit_appended(const ve::EditOp &op, bool notify_islands) override;

	// The mesher runs on its own thread and owns its local RenderingDevice there; see
	// MeshService. Nothing on the main thread touches that device.
	MeshService *mesh_ = nullptr;
	ve::ChunkResidency *chunks_ = nullptr;
	ColliderStreamer *colliders_ = nullptr;
	IslandManager *island_manager_ = nullptr;
	std::vector<float> physics_bubble_centers_;
	bool physics_ready_ = false;
	std::vector<std::pair<ve::IVec3, ve::IVec3>> pending_dirty_; // guarded by edit_mutex_
	// A hand-driven body pool for tests. Task 13's IslandManager owns the real one and
	// takes these over; until then this is what proves the body path works.
	std::vector<IslandBody *> test_bodies_;
	float last_physics_tick_ms_ = 0.0f; // diagnostic; see debug_perf_stats


	// Shader hot reload + beauty settings moved verbatim into RenderOrchestrator
	// (Task 14); VoxelWorld keeps one-line delegations and the ClassDB surface.

	bool extract_component(const std::vector<ve::IVec3> &cells, IslandExtractJob *job,
			std::vector<ve::CellBox> *boxes, ve::VolumeData *out);

protected:
	static void _bind_methods();

public:
	void _ready() override;
	void _process(double delta) override;
	void _exit_tree() override;
	~VoxelWorld() override;

	VoxelWorld();

	// Debug/test facade: all debug_* bindings live here (Phase 1 strangler split).
	VoxelDebugHooks *hooks();

	// The material registry, for the demo's picker. One dictionary per material, id
	// ascending. Bound rather than exposed as a property: it is constant for the process.
	Array material_table() const;
	// Subsystem wiring (spec §4). Phase-3 consumers (the debug facade) reach the
	// consolidation coordinator through it instead of through VoxelWorld members.
	VoxelContext &context() { return context_; }

	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	// Config setters/getters: write/read the store's config (setters through its named
	// per-field setters -- WorldStore has no whole-struct mutable escape hatch). Pre-init
	// writes take effect at the next ensure_initialized(); post-init they behave exactly
	// as before (pools never resize after creation).
	void set_atlas_bricks(Vector3i v) {
		store_->set_atlas_bricks({v.x, v.y, v.z});
	}
	Vector3i get_atlas_bricks() const {
		return {store_->config().atlas_bricks.x, store_->config().atlas_bricks.y,
				store_->config().atlas_bricks.z};
	}
	void set_max_region_slots(int v) { store_->set_max_region_slots(v); }
	int get_max_region_slots() const { return store_->config().max_region_slots; }
	void set_max_brick_jobs(int v) { store_->set_max_brick_jobs(v); }
	int get_max_brick_jobs() const { return store_->config().max_brick_jobs; }
	void set_max_override_bricks(int v) { store_->set_max_override_bricks(v); }
	int get_max_override_bricks() const { return store_->config().max_override_bricks; }
	void set_stream_radius_m(float v) { store_->set_stream_radius_m(v); }
	float get_stream_radius_m() const { return store_->config().stream_radius_m; }
	void set_occupancy_retention_m(float v) { store_->set_occupancy_retention_m(v); }
	float get_occupancy_retention_m() const { return store_->config().occupancy_retention_m; }
	void set_residency_radius_m(float v) { store_->set_residency_radius_m(v); }
	float get_residency_radius_m() const { return store_->config().residency_radius_m; }
	// Fraction of the engine's internal 3D resolution the near-field marcher runs at; the
	// composite upsamples its G-buffer to full size. The marcher is by far the most
	// per-pixel-expensive thing in the frame, so this is the frame budget's coarsest dial
	// and the one worth reaching for first on a GPU the default does not fit.
	// Read on the render thread, written from the main thread: atomic, like the effect
	// toggles next to it.
	void set_near_field_scale(float v) { context_.render->set_near_field_scale(v); }
	float get_near_field_scale() const { return context_.render->near_field_scale(); }

	void ensure_initialized();
	bool is_initialized() const { return context_.render->initialized(); }
	// Compiles assets/pipelines/default.pipeline into the GPU field override plus the
	// store's CPU generator. First successful load wins: shader-reload re-init must not
	// swap the generator under in-flight physics/mesh jobs (set_generator deletes the
	// old seam), so later calls are no-ops and pipeline edits take effect on fresh init.
	void load_terrain_pipeline();
	// One-line delegations into RenderOrchestrator (Task 13), where the lifetime state
	// lives now; kept so compositors, the debug facade and ClassDB compile unchanged.
	void shutdown_render_resources();
	// ClassDB-bound as "_shutdown_render_resources_on_render_thread": the render-thread
	// teardown Callable targets THIS node (an Object), so the binding must stay here.
	void shutdown_render_resources_on_render_thread();
	// Render effects acquire this guard before dereferencing VoxelWorld. _exit_tree() blocks
	// teardown until all callbacks that already acquired it have released their resources.
	bool try_begin_render_callback();
	void end_render_callback();
	void ensure_physics_initialized();
	void teardown_physics();
	int physics_tick(Vector3 center); // returns actions taken; Task 7 gives it a body
	void set_physics_enabled(bool v) { physics_enabled_ = v; }
	bool get_physics_enabled() const { return physics_enabled_; }
	void set_physics_center_path(const NodePath &p) { physics_center_path_ = p; }
	NodePath get_physics_center_path() const { return physics_center_path_; }
	void set_sun_light_path(const NodePath &p) { sun_light_path_ = p; }
	NodePath get_sun_light_path() const { return sun_light_path_; }
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
	void set_max_lod_pages(int v) { lod_->set_max_lod_pages(v); }
	int get_max_lod_pages() const { return lod_->max_lod_pages(); }
	void set_max_lod_chunk_records(int v) { lod_->set_max_lod_chunk_records(v); }
	int get_max_lod_chunk_records() const { return lod_->max_lod_chunk_records(); }
	void set_lod_builds_per_frame(int v) { lod_->set_lod_builds_per_frame(v); }
	int get_lod_builds_per_frame() const { return lod_->lod_builds_per_frame(); }
	void set_terrain_pipeline_path(const String &v) { terrain_pipeline_path_ = v; }
	String get_terrain_pipeline_path() const { return terrain_pipeline_path_; }

	// One-line delegations into RenderOrchestrator (Task 14 move); the ClassDB surface
	// and call sites compile unchanged. The effect/quality setters run on the main
	// thread, exactly as before the move.
	void set_quality_tier(int v);
	int get_quality_tier() const;
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	// The magnitude knobs (SSGI's gather shape, the emissive spill, the outline thresholds).
	// Same fail-soft contract as the toggles: an unknown name is ignored, not a crash.
	void set_effect_value(const String &name, float value);
	float get_effect_value(const String &name) const;
	// Spec §8 dev-build affordance: request a shader reload (latch, safe from _input).
	void request_shader_reload();

	int sun_cascade_count() const;
	void set_sun_cascade_min_level(bool v) { context_.render->set_sun_cascade_min_level(v); }
	bool get_sun_cascade_min_level() const { return context_.render->sun_cascade_min_level(); }
	RenderingDevice *rd() const; // one-line delegation into RenderOrchestrator
	bool set_grass_value(const String &name, float v);
	float get_grass_value(const String &name) const;
	MeshService *mesh_service() { return mesh_; }

	// One-line delegation into RenderOrchestrator (Task 13); also called by the debug
	// facade's forced-teardown probes. Every GPU object; CPU cores survive.
	void teardown_gpu();

	// Tool entry point (VoxelEditTool, Task 14). Main thread; takes edit_mutex(). One-line
	// delegation into WorldStore's spine so external callers compile unchanged.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
	// GDScript-bound as "append_edit" (Task 10 contract smoke test): parses ONE op from its
	// 32-byte ve::EditOp encoding -- the byte layout the tests' make_op helpers write --
	// and runs it through append_edit(). Returns the same {touched, rejected} Dictionary
	// shape VoxelEditTool reports, because GDScript cannot name ve::EditLog::AppendResult.
	Dictionary append_edit_op(const PackedByteArray &op_bytes);
	// Low-level append used by IslandManager to hold edit_mutex across a carve/restore
	// sequence. The caller MUST already hold edit_mutex(). Runs WorldStore's spine, then
	// applies the VoxelWorld-owned fan-out remainder (rejection stats, LoD dirty marks,
	// collider remesh queue) under the same single lock hold as before the split.
	ve::EditLog::AppendResult append_edit_locked(const ve::EditOp &op,
			bool notify_islands = true);
	// --- Task 8 hooks ---
	int64_t edit_seq() const { return store_->edit_seq(); }

	// Pre-init-only swap of the field-generation seam (spec §4, Task 10). One-line
	// delegation into WorldStore, which owns the generator; see WorldStore::set_generator
	// for the ownership/no-guard rationale. Used by future worldgen features.
	void set_generator(ve::FieldGenerator *generator) { store_->set_generator(generator); }


};

} // namespace godot
