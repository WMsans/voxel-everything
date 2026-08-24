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
#include "lod/lod_system.h"
#include "mesh/chunk_residency.h"
#include "physics/island_body.h"
#include "physics/island_manager.h"
#include "render/island_atlas.h"
#include "render/gpu_timings.h"
#include "render/orchestrator.h" // inline pass-graph delegations need the complete type
#include "render/consolidate_pass.h"
#include "shade/beauty_settings.h"
#include "world/edit_log.h"
#include "world/raycast.h"
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
class RaymarchPass;
class CompositePass;
class DeferredPass;
class SunShadowPass;
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
class BeautyCompositor;
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
	// island_manager_, initialized_, physics_ready_, test_bodies_, island uploads/desc
	// state, ...) plus 4 private helpers (drain_occupancy, render_probe_pixel,
	// gather_lod_ops, extract_component) -- audited at Task 16. JUSTIFICATION: every one
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
	// GPU pass graph + device ownership (Task 12): every pass pointer, the downsample
	// pipeline and main_rd_/local_rd_ live in RenderOrchestrator now; VoxelWorld keeps
	// one-line delegations so external callers compile unchanged.
	std::unique_ptr<RenderOrchestrator> render_;

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
	WorldStreamer *streamer_ = nullptr;
	int overflow_seen_ = 0;                   // sticky OR of frame overflow bits (tests)
	int edit_rejections_ = 0; // append fan-out rejection stat; read by debug_stream_stats

	void drain_occupancy() { store_->drain_occupancy(); } // one-line delegation (Task 9)
	// EditSink port satisfied for WorldStore's spine; adapter body forwards to today's
	// island-manager notification.
	void on_edit_appended(const ve::EditOp &op, bool notify_islands) override;
	bool render_probe_pixel(Vector3 origin, Vector3 dir);

	// The mesher runs on its own thread and owns its local RenderingDevice there; see
	// MeshService. Nothing on the main thread touches that device.
	MeshService *mesh_ = nullptr;
	ve::ChunkResidency *chunks_ = nullptr;
	ColliderStreamer *colliders_ = nullptr;
	IslandManager *island_manager_ = nullptr;
	mutable std::mutex island_mutex_; // also guards island_manager_ and island_slots_
	// Bytes on their way to a GPU pool. Filled on the main thread, drained on the render
	// thread by the compositor before it runs the streamer -- an op that names a volume must
	// never be evaluated before the volume is there. Since Task 6 the SDF/material/normal
	// bytes land ONCE in GpuAtlas's shared pools, indexed by the authoritative VOLUME slot;
	// an island upload additionally carries the atlas slot for its descriptor/mip entries.
	struct IslandUpload {
		int atlas_slot = -1;    // island mip/descriptor entry; -1 = field-volume only
		int volume_slot = -1;   // authoritative ve::VolumeSet slot (SDF/mat/normals stride)
		bool to_island_atlas = false; // true = also upload the island min-max mip
		ve::VolumeData data;
	};
	std::vector<IslandUpload> island_uploads_;
	// Volume slots whose compact-normal allocation must be freed on the render thread
	// (queued by release_volume_slot() when the authoritative copy is released).
	std::vector<int> pending_normal_releases_;
	// Debug-settable compact-normal budget; 0 = GpuAtlasConfig's default 32 MiB. Must be
	// set BEFORE the atlas is created; the pool never resizes after that.
	uint32_t normal_pool_bytes_ = 0;
	std::atomic<bool> islands_enabled_{true};
	std::atomic<bool> near_field_enabled_{true};
	std::atomic<float> near_field_scale_{0.66f};
	int island_slots_ = 0; // high-water mark, not a population; guarded by island_mutex_
	BeautyCompositor *beauty_compositor_ = nullptr;
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

	// Owns the LoD runtime (Task 15): THE lod mutex, tree/walk/page-map/pool state plus
	// tick/fade-band/op-gathering live in LodSystem now; VoxelWorld keeps one-line
	// delegations so the compositor and debug facade compile unchanged. Created BEFORE
	// RenderOrchestrator, whose teardown interleaves with its pool/tree/page maps via
	// address-of slots (handles-only collaborators).
	std::unique_ptr<LodSystem> lod_;

	bool initialized_ = false;
	// HiZ async-readback end state captured by RenderOrchestrator's teardown (handle-
	// injected); read by the debug facade after a shutdown.
	bool last_hiz_readback_was_pending_ = false;
	bool last_hiz_readback_was_drained_ = true;

	// Shader hot reload + beauty settings moved verbatim into RenderOrchestrator
	// (Task 14); VoxelWorld keeps one-line delegations and the ClassDB surface.

	// Gathers the ops that can affect a LoD chunk: its AABB padded by two cells, flattened
	// across regions in global append order, truncated to a chronological prefix (M4 errata 1).
	// One-line delegation into LodSystem's gather_ops (Task 15 move); called by the debug
	// facade (friend) exactly as it called the world's own body before.
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

	VoxelWorld();

	// Debug/test facade: all debug_* bindings live here (Phase 1 strangler split).
	VoxelDebugHooks *hooks();
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
	void set_world_origin_bricks(Vector3i v) {
		store_->set_world_origin_bricks({v.x, v.y, v.z});
	}
	Vector3i get_world_origin_bricks() const {
		return {store_->config().world_origin_bricks.x, store_->config().world_origin_bricks.y,
				store_->config().world_origin_bricks.z};
	}
	void set_world_size_regions(Vector3i v) {
		store_->set_world_size_regions({v.x, v.y, v.z});
	}
	Vector3i get_world_size_regions() const {
		return {store_->config().world_size_regions.x, store_->config().world_size_regions.y,
				store_->config().world_size_regions.z};
	}
	void set_residency_radius_m(float v) { store_->set_residency_radius_m(v); }
	float get_residency_radius_m() const { return store_->config().residency_radius_m; }
	// Fraction of the engine's internal 3D resolution the near-field marcher runs at; the
	// composite upsamples its G-buffer to full size. The marcher is by far the most
	// per-pixel-expensive thing in the frame, so this is the frame budget's coarsest dial
	// and the one worth reaching for first on a GPU the default does not fit.
	// Read on the render thread, written from the main thread: atomic, like the effect
	// toggles next to it.
	void set_near_field_scale(float v) {
		near_field_scale_.store(v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v), std::memory_order_relaxed);
	}
	float get_near_field_scale() const { return near_field_scale_.load(std::memory_order_relaxed); }

	void ensure_initialized();
	bool is_initialized() const { return initialized_; }
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
	void set_lod_builds_per_frame(int v) { lod_->set_lod_builds_per_frame(v); }
	int get_lod_builds_per_frame() const { return lod_->lod_builds_per_frame(); }

	// One-line delegations into RenderOrchestrator (Task 14 move); the ClassDB surface
	// and call sites compile unchanged. The effect/quality setters run on the main
	// thread, exactly as before the move.
	void set_quality_tier(int v);
	int get_quality_tier() const;
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	// Spec §8 dev-build affordances: request a shader reload (latch, safe from _input) and
	// report what the last reload did. debug_pump_shader_reload() lets tests step the render
	// callback's reload work directly.
	void request_shader_reload();
	void pump_shader_reload();
	// Returns an immutable value snapshot (RenderOrchestrator-owned mutex since Task 14).
	// Render callbacks must take this once per frame and pass the copy through their work;
	// the mutex is never held during render work.
	ve::BeautySettings beauty_settings() const;
	// Task 14 temporary hook-facing surface (deleted with the debug facade's world_ back-
	// reference): single-mutex-hold copies matching the pre-move debug_* body shapes.
	void reload_snapshot(int *out_count, bool *out_last_ok, String *out_last_error) const {
		context_.render->reload_snapshot(out_count, out_last_ok, out_last_error);
	}
	void beauty_snapshot(ve::BeautySettings *out_settings, int *out_tier) const {
		context_.render->beauty_snapshot(out_settings, out_tier);
	}
	void set_normal_roughness_state(int state) { context_.render->set_normal_roughness_state(state); }
	int get_normal_roughness_state() const { return context_.render->normal_roughness_state(); }
	void set_beauty_compositor(BeautyCompositor *effect) { beauty_compositor_ = effect; }

	// One-line delegations into LodSystem (Task 15 move); the compositor, the debug facade
	// and ClassDB compile unchanged.
	void lod_tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ);
	// Push the current walk's page list (with per-page quad counts) into the raster pass.
	void prepare_lod_raster();
	void prepare_lod_shadow_raster();
	RenderingDevice *rd() const; // one-line delegation into RenderOrchestrator
	GpuTimings *gpu_timings() { return context_.render->gpu_timings(); }
	ve::WorldBounds world_bounds() const;

	GpuAtlas *atlas() { return context_.render->atlas(); }
	MaterialAtlas *material_atlas() { return context_.render->materials(); }
	IslandAtlas *islands() { return context_.render->islands(); }
	// High-water mark, not a population: the shader masks off bits at or above it and then
	// tests each remaining slot's descriptor for dim >= 2, so a dead slot below the mark
	// costs one branch and nothing else. Non-inline: the render thread calls this and must
	// take island_mutex_ before touching island_manager_ / island_slots_.
	int island_slot_count() const;
	WorldStreamer *streamer() { return streamer_; }
	ve::EditLog *edit_log() { return store_->edit_log(); }
	ve::VolumeSet &volumes() { return store_->volumes(); }
	RaymarchPass *raymarch_pass() { return context_.render->raymarch_pass(); }
	IslandCullPass *island_cull() { return context_.render->island_cull(); }
	CompositePass *composite_pass() { return context_.render->composite_pass(); }
	DeferredPass *deferred_pass() { return context_.render->deferred_pass(); }
	SunShadowPass *sun_shadow_pass() { return context_.render->sun_shadow_pass(); }
	InjectPass *inject_pass() { return context_.render->inject_pass(); }
	LodPool *lod_pool() { return context_.lod->pool(); }
	LodRasterPass *lod_raster_pass() { return context_.render->lod_raster_pass(); }
	LodCullPass *lod_cull_pass() { return context_.render->lod_cull_pass(); }
	HizPass *hiz_pass() { return context_.render->hiz_pass(); }
	GBuffer *gbuffer() { return context_.render->gbuffer(); }
	CameraUbo *beauty_camera() { return context_.render->beauty_camera(); }
	ContactShadowPass *contact_shadow_pass() { return context_.render->contact_shadow_pass(); }
	SsgiPass *ssgi_pass() { return context_.render->ssgi_pass(); }
	SsaoPass *ssao_pass() { return context_.render->ssao_pass(); }
	SsrPass *ssr_pass() { return context_.render->ssr_pass(); }
	OutlinePass *outline_pass() { return context_.render->outline_pass(); }
	// Region/gen passes have no pre-split accessor; added for the debug facade, which
	// pokes them directly today (Task 12 moves their pointers into RenderOrchestrator).
	RegionPass *region_pass() { return context_.render->region_pass(); }
	BrickGenPass *gen_pass() { return context_.render->gen_pass(); }
	// The debug facade's local-device probe (debug_local_rd).
	RenderingDevice *local_rd() const { return context_.render->local_rd(); }
	const float *prev_view_proj() const { return context_.render->prev_view_proj(); }
	bool has_history() const { return context_.render->has_history(); }
	uint32_t beauty_frame() const { return context_.render->beauty_frame(); }
	void finish_beauty_frame(const float view_proj[16]);
	std::mutex &edit_mutex() { return store_->edit_mutex(); }
	MeshService *mesh_service() { return mesh_; }
	// Releases an authoritative volume slot AND queues the render-thread teardown of its
	// compact-normal allocation. Pinned slots are refused by VolumeSet::release() and keep
	// their normals (a pasted volume-add still names them). Returns release()'s result.
	bool release_volume_slot(int slot);
	void queue_island_upload(int atlas_slot, int volume_slot, const ve::VolumeData &d);
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

	// One-line delegation into RenderOrchestrator's downsample pipeline (Task 12 move;
	// public since Task 13 so BeautyCompositor no longer needs to be a friend).
	bool downsample_history(RenderingDevice *rd, RID src, GBuffer &gb);
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
	int override_table_for_region(ve::IVec3 region) const;

	// --- Task 8 hooks ---
	// One-line delegations into WorldStore so external callers compile unchanged.
	ve::OccupancyGrid &occupancy() { return store_->occupancy(); }
	int64_t edit_seq() const { return store_->edit_seq(); }

	// Pre-init-only swap of the field-generation seam (spec §4, Task 10). One-line
	// delegation into WorldStore, which owns the generator; see WorldStore::set_generator
	// for the ownership/no-guard rationale. Used by future worldgen features.
	void set_generator(ve::FieldGenerator *generator) { store_->set_generator(generator); }

	bool snapshot_field_sources(const std::vector<ve::EditOp> &ops, ve::IVec3 brick_lo, ve::IVec3 brick_hi, ve::FieldSourceSnapshot *out) const;

	// The near/far seam for this frame -- one-line delegation into LodSystem (Task 15),
	// which owns the fade band now.
	void lod_fade_band(float *fade_start, float *fade_end) const;

};

} // namespace godot
