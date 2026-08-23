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
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <utility>
#include <vector>
#include <tuple>
#include "connectivity/occupancy.h"
#include "debug/hooks.h"
#include "generator/volume_set.h"
#include "lod/lod_tree.h"
#include "mesh/chunk_residency.h"
#include "physics/island_body.h"
#include "physics/island_manager.h"
#include "render/island_atlas.h"
#include "render/gpu_timings.h"
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
class VoxelWorld;
bool voxel_compositor_callbacks_enabled();
bool voxel_try_begin_compositor_callback(const NodePath &world_path, VoxelWorld **world);
void voxel_compositor_callbacks_ready(VoxelWorld *world);
void voxel_compositor_callbacks_shutdown_started(VoxelWorld *world);

class GpuAtlas;
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
class SsrPass;
class OutlinePass;
class BeautyCompositor;
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
	friend class VoxelDebugHooks;
	friend class BeautyCompositor;
	friend bool voxel_try_begin_compositor_callback(const NodePath &, VoxelWorld **);
	friend void voxel_compositor_callbacks_ready(VoxelWorld *);
	friend void voxel_compositor_callbacks_shutdown_started(VoxelWorld *);

	VoxelDebugHooks *debug_hooks_ = nullptr;

	bool use_local_device_ = false;

	Vector3i atlas_bricks_ = Vector3i(64, 32, 32);
	int max_region_slots_ = 512;
	int max_brick_jobs_ = 16384;
	int max_override_bricks_ = 8192;
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
	std::atomic<bool> islands_enabled_{true};
	std::atomic<bool> near_field_enabled_{true};
	int island_slots_ = 0; // high-water mark, not a population; guarded by island_mutex_
	IslandCullPass *island_cull_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *gen_pass_ = nullptr;
	RaymarchPass *raymarch_pass_ = nullptr;
	CompositePass *composite_pass_ = nullptr;
	DeferredPass *deferred_pass_ = nullptr;
	SunShadowPass *sun_shadow_pass_ = nullptr;
	InjectPass *inject_pass_ = nullptr;
	LodRasterPass *lod_raster_pass_ = nullptr;
	LodCullPass *lod_cull_pass_ = nullptr;
	HizPass *hiz_pass_ = nullptr;
	GBuffer *gbuffer_ = nullptr;
	CameraUbo *beauty_camera_ = nullptr;
	ContactShadowPass *contact_shadow_pass_ = nullptr;
	SsgiPass *ssgi_pass_ = nullptr;
	SsrPass *ssr_pass_ = nullptr;
	OutlinePass *outline_pass_ = nullptr;
	BeautyCompositor *beauty_compositor_ = nullptr;
	GpuTimings gpu_timings_;
	float prev_view_proj_[16] = {};
	bool has_history_ = false;
	uint32_t beauty_frame_ = 0;
	int normal_roughness_state_ = -1;
	RID downsample_shader_, downsample_pipeline_, downsample_sampler_, downsample_uset_;
	RID downsample_src_, downsample_dst_;
	// CPU cores outlive the GPU objects: a re-init re-streams the same world, edits
	// included. This is also what a future save/reload will do (saves ARE the edit log).
	ve::EditLog *edit_log_ = nullptr;
	ve::OverrideStore *overrides_ = nullptr;
	std::map<std::tuple<int, int, int>, int> override_tables_;
	// The authoritative copy of every stored volume. Owned here because it outlives the GPU
	// objects exactly as the edit log does: a re-init re-uploads the same rubble.
	ve::VolumeSet volumes_;
	ve::RegionResidency *residency_ = nullptr;
	WorldStreamer *streamer_ = nullptr;
	std::mutex edit_mutex_;                   // guards edit_log_ + pending_edits_
	std::vector<PendingEdit> pending_edits_;  // appended by tools, drained by the streamer
	int overflow_seen_ = 0;                   // sticky OR of frame overflow bits (tests)

	// Consolidation is deliberately one-region-at-a-time. The worker owns the bake; the main
	// thread owns this queue and publishes the completed transaction between frames.
	std::vector<ve::IVec3> consolidation_queue_;
	bool consolidation_in_flight_ = false;
	ConsolidateJob consolidation_job_;
	int consolidation_table_ = -1;
	int consolidation_old_table_ = -1;
	std::vector<std::pair<int, int>> consolidation_old_entries_;
	std::vector<std::pair<int, int>> consolidation_entries_;
	std::vector<int> consolidation_old_slots_;
	std::vector<ve::OverrideBrick> consolidation_old_bricks_;
	std::vector<ve::IVec3> consolidation_newly_acquired_;
	std::vector<int> consolidation_slots_;
	std::vector<ve::OverrideBrick> consolidation_baked_;
	bool consolidation_publish_in_flight_ = false;
	int consolidation_count_ = 0;
	int consolidation_refusals_ = 0;
	int consolidation_queue_refusals_ = 0;
	int edit_rejections_ = 0;
	bool consolidation_queue_refusal_logged_ = false;

	ve::OccupancyGrid occupancy_;              // main thread only
	std::mutex occupancy_mutex_;               // guards occupancy_inbox_
	std::vector<OccupancyBlock> occupancy_inbox_;
	// Monotonic; bumped by every accepted edit. The streamer stamps each occupancy readback
	// with it so IslandManager (Task 13) can tell whether a window's cells are new enough to
	// act on, rather than running connectivity against a picture of the world from before
	// the blast.
	std::atomic<int64_t> edit_seq_{0};
	void drain_occupancy();                    // inbox -> grid
	void pump_consolidation();
	bool queue_consolidation(ve::IVec3 region); // edit_mutex_ must be held
	void requeue_consolidation_locked(ve::IVec3 region);
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
	// Guards lod_tree_, lod_walk_, lod_pages_of_, lod_page_quads_, and lod_pool_ state
	// between the render thread (lod_tick) and main/tool threads (mark_dirty, debug stats).
	// Lock order is edit_mutex_ -> lod_mutex_: lod_tick never holds lod_mutex_ while it calls
	// gather_lod_ops (which takes edit_mutex_), so append_edit_locked can safely take
	// lod_mutex_ while already holding edit_mutex_.
	std::mutex lod_mutex_;
	using LodKey = ve::LodKey;
	ve::LodTree *lod_tree_ = nullptr;
	LodPool *lod_pool_ = nullptr;
	uint32_t lod_frame_ = 0;
	ve::LodWalkResult lod_walk_;
	std::map<LodKey, std::vector<int>> lod_pages_of_;
	std::map<int, int> lod_page_quads_; // page -> number of quads stored in that page
	std::set<LodKey> lod_overflow_logged_; // once-per-chunk overflow diagnostics
	int lod_pressure_ = 0;

	// --- M6 beautification settings (Task 3) ---
	// Setters run on the main thread; render callbacks take a value snapshot through
	// beauty_settings() rather than retaining a reference to this mutable state.
	mutable std::mutex beauty_mutex_;
	int quality_tier_ = static_cast<int>(ve::QualityTier::kHigh);
	ve::BeautySettings beauty_ = ve::settings_for_tier(ve::QualityTier::kHigh);

	void ensure_lod(); // lazy: creates/initializes lod_tree_ + lod_pool_ on first use
	// Assumes lod_mutex_ is held; emits the real page list for the current lod_walk_.
	void prepare_lod_raster_locked();

	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	bool initialized_ = false;
	mutable std::mutex render_lifetime_mutex_;
	std::condition_variable render_lifetime_cv_;
	bool render_shutting_down_ = false;
	bool render_teardown_deferred_ = false;
	int render_callbacks_ = 0;
	std::condition_variable gpu_teardown_cv_;
	bool gpu_teardown_done_ = false;
	bool last_hiz_readback_was_pending_ = false;
	bool last_hiz_readback_was_drained_ = true;

	// Shader hot reload (spec §8). request_shader_reload() only sets the latch; the render
	// callback pumps it, pre-flights every shader, and only then tears down and rebuilds the
	// GPU objects so a bad shader never kills the last-known-good pipelines.
	std::atomic<bool> reload_requested_{false};
	std::mutex reload_mutex_;
	int reload_count_ = 0;
	bool reload_last_ok_ = true;
	String reload_last_error_;
	bool preflight_shaders(RenderingDevice *rd, String *out_error);

	void teardown_gpu(); // every GPU object; CPU cores survive
	void shutdown_render_resources_on_render_thread();
	bool initialize_downsample(RenderingDevice *rd);
	void teardown_downsample();
	bool ensure_downsample_set(RenderingDevice *rd, RID src, RID dst);
	bool downsample_history(RenderingDevice *rd, RID src, GBuffer &gb);
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

	// Debug/test facade: all debug_* bindings live here (Phase 1 strangler split).
	VoxelDebugHooks *hooks();

	void set_use_local_device(bool v) { use_local_device_ = v; }
	bool get_use_local_device() const { return use_local_device_; }
	void set_atlas_bricks(Vector3i v) { atlas_bricks_ = v; }
	Vector3i get_atlas_bricks() const { return atlas_bricks_; }
	void set_max_region_slots(int v) { max_region_slots_ = v; }
	int get_max_region_slots() const { return max_region_slots_; }
	void set_max_brick_jobs(int v) { max_brick_jobs_ = v; }
	int get_max_brick_jobs() const { return max_brick_jobs_; }
	void set_max_override_bricks(int v) {
		const int requested = std::max(v, 0);
		max_override_bricks_ = overrides_ ? std::min(requested, overrides_->capacity()) : requested;
	}
	int get_max_override_bricks() const { return max_override_bricks_; }
	void set_world_origin_bricks(Vector3i v) { world_origin_bricks_ = v; }
	Vector3i get_world_origin_bricks() const { return world_origin_bricks_; }
	void set_world_size_regions(Vector3i v) { world_size_regions_ = v; }
	Vector3i get_world_size_regions() const { return world_size_regions_; }
	void set_residency_radius_m(float v) { residency_radius_m_ = v; }
	float get_residency_radius_m() const { return residency_radius_m_; }

	void ensure_initialized();
	bool is_initialized() const { return initialized_; }
	void shutdown_render_resources();
	// Render effects acquire this guard before dereferencing VoxelWorld. _exit_tree() blocks
	// teardown until all callbacks that already acquired it have released their resources.
	bool try_begin_render_callback();
	void end_render_callback();
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

	void set_quality_tier(int v);
	int get_quality_tier() const;
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	// Spec §8 dev-build affordances: request a shader reload (latch, safe from _input) and
	// report what the last reload did. debug_pump_shader_reload() lets tests step the render
	// callback's reload work directly.
	void request_shader_reload();
	void pump_shader_reload();
	// Returns an immutable value snapshot. Render callbacks must take this once per frame and
	// pass the copy through their work; the mutex is never held during render work.
	ve::BeautySettings beauty_settings() const;
	void set_normal_roughness_state(int state) { normal_roughness_state_ = state; }
	void set_beauty_compositor(BeautyCompositor *effect) { beauty_compositor_ = effect; }

	void lod_tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ);
	// Push the current lod_walk_ page list (with per-page quad counts) into the raster pass.
	void prepare_lod_raster();
	void prepare_lod_shadow_raster();
	RenderingDevice *rd() const;
	GpuTimings *gpu_timings() { return &gpu_timings_; }
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
	DeferredPass *deferred_pass() { return deferred_pass_; }
	SunShadowPass *sun_shadow_pass() { return sun_shadow_pass_; }
	InjectPass *inject_pass() { return inject_pass_; }
	LodPool *lod_pool() { return lod_pool_; }
	LodRasterPass *lod_raster_pass() { return lod_raster_pass_; }
	LodCullPass *lod_cull_pass() { return lod_cull_pass_; }
	HizPass *hiz_pass() { return hiz_pass_; }
	GBuffer *gbuffer() { return gbuffer_; }
	CameraUbo *beauty_camera() { return beauty_camera_; }
	ContactShadowPass *contact_shadow_pass() { return contact_shadow_pass_; }
	SsgiPass *ssgi_pass() { return ssgi_pass_; }
	SsrPass *ssr_pass() { return ssr_pass_; }
	OutlinePass *outline_pass() { return outline_pass_; }
	const float *prev_view_proj() const { return prev_view_proj_; }
	bool has_history() const { return has_history_; }
	uint32_t beauty_frame() const { return beauty_frame_; }
	void finish_beauty_frame(const float view_proj[16]);
	std::mutex &edit_mutex() { return edit_mutex_; }
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


	// Tool entry point (VoxelEditTool, Task 14). Main thread; takes edit_mutex_.
	ve::EditLog::AppendResult append_edit(const ve::EditOp &op);
	// Low-level append used by IslandManager to hold edit_mutex_ across a carve/restore
	// sequence. The caller MUST already hold edit_mutex_().
	// `notify_islands` is false only for the island manager's own crumble carve: the matter
	// it removes was already labelled UNANCHORED, so nothing that was holding on can be
	// loosened by its going, and enqueueing a window would relabel the same neighbourhood
	// every time a speck of sub-voxel dust is swept up.
	int override_table_for_region(ve::IVec3 region) const;

	ve::EditLog::AppendResult append_edit_locked(const ve::EditOp &op,
			bool notify_islands = true);


	// --- Task 8 hooks ---
	ve::OccupancyGrid &occupancy() { return occupancy_; }
	int64_t edit_seq() const { return edit_seq_.load(std::memory_order_relaxed); }





	bool snapshot_field_sources(const std::vector<ve::EditOp> &ops, ve::IVec3 brick_lo, ve::IVec3 brick_hi, ve::FieldSourceSnapshot *out) const;




	// The near/far seam for this frame, derived from how far the near field's brick data is
	// actually complete. One source of truth: the composite, the LoD raster and the LoD
	// build gate must all fade at the same two distances or the band belongs to no field.
	void lod_fade_band(float *fade_start, float *fade_end) const;


};

} // namespace godot
