#pragma once
// RenderOrchestrator — the ~30 pass pointers, the downsample pipeline, and the
// RenderingDevice ownership extracted from VoxelWorld (spec Phase 4a / Task 12).
// Needs GPU/Godot runtime; NOT in the native-test pure_sources (src/render/*.cpp
// is never globbed there -- see SConstruct).
//
// Construction/teardown ORDER of the GPU objects below is load-bearing (the
// CPU-outlives-GPU invariant): ensure_gpu_graph(), teardown_gpu() and the
// teardown_*() halves preserve the exact allocation/deallocation sequence
// VoxelWorld used before the split. Compositor admission/lifetime moved here
// verbatim in Phase 4b (Task 13); shader reload + beauty snapshot + timings
// followed verbatim in Phase 4c (Task 14).
//
// No VoxelWorld* lives here: collaborators arrive as injected handles/addresses
// (same rule as ConsolidationCoordinator's Collaborators -- the store/spine
// objects are created lazily and destroyed across teardown cycles, so their
// addresses are re-read at every use instead of caching stranded pointers).
// The single exception is callback_owner (the owning VoxelWorld node as an
// Object): it exists only as the Callable target for the render-thread teardown
// dispatch, and cannot dangle because this orchestrator is a member of that
// node and dies with it.
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <vector>

#include "lod/lod_tree.h" // ve::LodKey: teardown clears the world's LoD page maps
#include "render/gpu_timings.h"
#include "shade/beauty_settings.h"
#include "world/region.h"

namespace godot {

// Shared name of the ClassDB method binding (voxel_world.cpp -- the Callable must
// target the bound node) and of the render-thread teardown Callable dispatched here.
// Task 13 minor: one constant so the two sites cannot drift apart.
inline constexpr char kShutdownRenderResourcesOnRenderThread[] =
		"_shutdown_render_resources_on_render_thread";

class RenderingDevice;
class WorldStore;
class WorldStreamer;
class GpuAtlas;
class MaterialAtlas;
class IslandAtlas;
class IslandCullPass;
class RegionPass;
class BrickGenPass;
class RaymarchPass;
class CompositePass;
class DeferredPass;
class SunShadowPass;
class InjectPass;
class LodRasterPass;
class LodCullPass;
class HizPass;
class GBuffer;
class CameraUbo;
class SunUbo;
class ContactShadowPass;
class SsgiPass;
class SsaoPass;
class SsrPass;
class OutlinePass;
class LodPool;
class Object;

class RenderOrchestrator {
public:
	struct Collaborators {
		// Device-selection seam: use_local_device_ stays a VoxelWorld property.
		const bool *use_local_device = nullptr;
		// Config/residency spine steps sit mid-sequence in ensure_gpu_graph().
		WorldStore *store = nullptr;
		// Created INSIDE the verbatim init sequence; the slot is VoxelWorld's field.
		WorldStreamer **streamer = nullptr;
		// Debug-settable atlas budget read at GpuAtlasConfig time (0 = default).
		const uint32_t *normal_pool_bytes = nullptr;
		// Teardown captures HiZ's async-readback end state for the debug facade.
		bool *last_hiz_readback_was_pending = nullptr;
		bool *last_hiz_readback_was_drained = nullptr;
		// --- Task 13 (lifetime/admission + teardown interleaving) ---
		// World-owned flags/state teardown_gpu() touches BETWEEN its three halves
		// and after them. Addresses only, re-read at every use.
		bool *initialized = nullptr;        // cleared last by teardown_gpu(), as before
		std::mutex *island_mutex = nullptr; // guards *island_slots during teardown
		int *island_slots = nullptr;        // high-water mark reset under *island_mutex
		LodPool **lod_pool = nullptr;       // pool -> tree -> page maps, post-atlas
		ve::LodTree **lod_tree = nullptr;
		std::map<ve::LodKey, std::vector<int>> *lod_pages_of = nullptr;
		std::map<int, int> *lod_page_quads = nullptr;
		std::set<ve::LodKey> *lod_overflow_logged = nullptr;
		// Callable target for the queued render-thread teardown (see class comment).
		Object *callback_owner = nullptr;
		// --- Task 14 ---
		// set_effect_enabled()/get_effect_enabled() route the two atomic toggles that
		// stayed world properties (they gate non-beauty behavior too). Addresses only.
		std::atomic<bool> *islands_enabled = nullptr;
		std::atomic<bool> *near_field_enabled = nullptr;
		// pump_shader_reload()'s re-init arm: VoxelWorld::ensure_initialized() (it owns
		// the CPU-side init ordering around the GPU half this class moved in Task 12).
		// Generic thunk + user-data, not a stored VoxelWorld*: the world registers a
		// captureless static trampoline at construction, exactly like callback_owner
		// above is registered for its single Callable purpose.
		void (*ensure_initialized_thunk)(void *) = nullptr;
		void *ensure_initialized_self = nullptr;
	};

	explicit RenderOrchestrator(Collaborators handles);

	// --- compositor admission/lifetime (moved VERBATIM from VoxelWorld, Task 13) ---
	// The lifetime mutex/cv pair below serializes compositor-callback admission against
	// shutdown exactly as VoxelWorld did: same latch checks, same cv waits/signals, same
	// deferred-teardown handoff to the render thread. Callers reach these through
	// VoxelWorld's one-line delegations or the free admission functions.
	bool try_begin_render_callback();
	void end_render_callback();
	// Admission transitions taken by the free admission functions while they hold the
	// global admission lock (lock order: g_voxel_compositor_admission_mutex ->
	// render_lifetime_mutex_, unchanged from the pre-move bodies).
	void reopen_admission();  // voxel_compositor_callbacks_ready()
	void close_admission();   // voxel_compositor_callbacks_shutdown_started()
	// ensure_initialized()/pump_shader_reload()'s gate, verbatim: refuse new init/reload
	// work once shutdown started. Takes render_lifetime_mutex_ for the check, as before.
	bool shutdown_in_progress();
	// Addresses consumed by ConsolidationCoordinator's Collaborators (it inspects the
	// shutting-down flag under this exact mutex from its worker thread).
	std::mutex *render_lifetime_mutex_slot() { return &render_lifetime_mutex_; }
	const bool *render_shutting_down_slot() const { return &render_shutting_down_; }

	// --- shader hot reload (spec §8), moved VERBATIM from VoxelWorld (Task 14) ---
	// request_shader_reload() only sets the latch; the render callback pumps it,
	// pre-flights every shader (preflight_shaders below), and only then tears down and
	// rebuilds the GPU objects so a bad shader never kills the last-known-good pipelines.
	void request_shader_reload();
	void pump_shader_reload();
	// Debug-facade snapshot: one reload_mutex_ hold copying all three bookkeeping fields,
	// the exact hold shape of the pre-move debug_shader_reload_stats body.
	void reload_snapshot(int *out_count, bool *out_last_ok, String *out_last_error) const;

	// --- beauty settings snapshot (moved VERBATIM from VoxelWorld, Task 14) ---
	// Setters run on the main thread; render callbacks take value snapshots through
	// beauty_settings() rather than retaining a reference to this mutable state; the
	// mutex is never held during render work.
	void set_quality_tier(int v);
	int quality_tier() const;
	void set_effect_enabled(const String &name, bool on);
	bool get_effect_enabled(const String &name) const;
	// Returns an immutable value snapshot. Render callbacks must take this once per frame
	// and pass the copy through their work; the mutex is never held during render work.
	ve::BeautySettings beauty_settings() const;
	// One beauty_mutex_ hold copying settings + tier together -- the exact hold shape of
	// the pre-move debug_beauty_settings body.
	void beauty_snapshot(ve::BeautySettings *out_settings, int *out_tier) const;

	// Outcome of the GPU-half of ensure_initialized():
	//   kOk          -- graph complete; caller sets its initialized_ flag.
	//   kAtlasFailed -- the atlas itself refused; ONLY the half-built atlas exists
	//                   (already deleted, as before) -- no full teardown needed.
	//   kFailed      -- a later stage refused; a partial graph exists and the
	//                   caller must run its full teardown_gpu() exactly where the
	//                   pre-split body did.
	enum class GpuInitResult { kOk, kAtlasFailed, kFailed };

	// --- device ownership ---
	// ensure_initialized()'s first half, verbatim: create the local device or
	// fetch the main one, then report whichever applies.
	RenderingDevice *acquire_device();
	RenderingDevice *rd() const; // local or main per *use_local_device_
	bool has_main_device() const { return main_rd_ != nullptr; }
	RenderingDevice *local_rd() const { return local_rd_; }
	// _exit_tree()'s device drop, verbatim: the owned local device is deleted,
	// the borrowed main pointer merely forgotten.
	void release_devices();

	// Address-of slots for collaborators (ConsolidationCoordinator wiring) that
	// re-read lazily-created objects at every use.
	GpuAtlas **atlas_slot() { return &atlas_; }
	RenderingDevice **main_rd_slot() { return &main_rd_; }
	RenderingDevice **local_rd_slot() { return &local_rd_; }

	// --- accessors: one per moved pass pointer ---
	GpuAtlas *atlas() { return atlas_; }
	MaterialAtlas *materials() { return materials_; }
	IslandAtlas *islands() { return islands_; }
	IslandCullPass *island_cull() { return island_cull_; }
	RegionPass *region_pass() { return region_pass_; }
	BrickGenPass *gen_pass() { return gen_pass_; }
	RaymarchPass *raymarch_pass() { return raymarch_pass_; }
	CompositePass *composite_pass() { return composite_pass_; }
	DeferredPass *deferred_pass() { return deferred_pass_; }
	SunShadowPass *sun_shadow_pass() { return sun_shadow_pass_; }
	InjectPass *inject_pass() { return inject_pass_; }
	LodRasterPass *lod_raster_pass() { return lod_raster_pass_; }
	LodCullPass *lod_cull_pass() { return lod_cull_pass_; }
	HizPass *hiz_pass() { return hiz_pass_; }
	GBuffer *gbuffer() { return gbuffer_; }
	CameraUbo *beauty_camera() { return beauty_camera_; }
	SunUbo *sun_ubo() { return sun_ubo_; }
	ContactShadowPass *contact_shadow_pass() { return contact_shadow_pass_; }
	SsgiPass *ssgi_pass() { return ssgi_pass_; }
	SsaoPass *ssao_pass() { return ssao_pass_; }
	SsrPass *ssr_pass() { return ssr_pass_; }
	OutlinePass *outline_pass() { return outline_pass_; }
	GpuTimings *gpu_timings() { return &gpu_timings_; }

	// --- history/beauty frame state (moved with the pass graph) ---
	const float *prev_view_proj() const { return prev_view_proj_; }
	bool has_history() const { return has_history_; }
	uint32_t beauty_frame() const { return beauty_frame_; }
	int normal_roughness_state() const { return normal_roughness_state_; }
	void set_normal_roughness_state(int state) { normal_roughness_state_ = state; }
	void finish_beauty_frame(const float view_proj[16]);

	// --- downsample pipeline (moved verbatim) ---
	bool initialize_downsample(RenderingDevice *device);
	void teardown_downsample();
	bool downsample_history(RenderingDevice *device, RID src, GBuffer &gb);

	// --- GPU-half of ensure_initialized(): construction order VERBATIM ---
	GpuInitResult ensure_gpu_graph(RenderingDevice *device);
	// --- teardown halves of VoxelWorld::teardown_gpu(): destruction order VERBATIM ---
	// composite/inject/deferred/sun-shadow deletes -> Hi-Z level0 release ->
	// downsample teardown -> contact/ssr/outline/ssgi/beauty-camera/gbuffer/
	// raymarch/lod-raster/lod-cull deletes -> Hi-Z teardown (+ readback capture)
	// -> materials/gen-pass/region-pass deletes.
	void teardown_render_passes();
	// island_cull_ then islands_ (between clear_residency() and the atlas delete).
	void teardown_island_graph();
	void teardown_atlas_pool();
	// The history resets sat after the LoD page-map clears in teardown_gpu(); the
	// state moved here, so the caller invokes this at the same relative position.
	void reset_history_state();

	// --- shutdown / teardown drivers (moved VERBATIM from VoxelWorld, Task 13) ---
	// Every GPU object; CPU cores survive. The interleaved world-owned steps (streamer
	// drain/delete, residency clear, island high-water mark, LoD pool/tree/page maps)
	// ride along via Collaborator addresses, so the deallocation ORDER is identical to
	// the pre-split body statement for statement.
	void teardown_gpu();
	// Queued onto the render thread by shutdown_render_resources(); signals
	// gpu_teardown_cv_ when the GPU half is gone. Also runs directly when the caller
	// already is on the render thread or owns a local device.
	void shutdown_render_resources_on_render_thread();
	// Closes compositor admission, drains in-flight callbacks, then tears the GPU graph
	// down -- on the render thread via Callable(callback_owner, ...) when the main
	// device is in play, otherwise inline. Exact pre-move sequence preserved.
	void shutdown_render_resources();
	// Shader hot-reload pre-flight (spec §8): compiles every res://shaders/*.glsl on
	// `rd` WITHOUT creating pipelines. A false return leaves out_error set and the
	// caller keeps the old pipelines. Body moved verbatim from VoxelWorld.
	bool preflight_shaders(RenderingDevice *rd, String *out_error);

private:
	ve::WorldBounds world_bounds() const; // store-config projection, as VoxelWorld's
	bool ensure_downsample_set(RenderingDevice *device, RID src, RID dst);

	Collaborators handles_;

	// Member ORDER mirrors the pre-split block in voxel_world.h.
	GpuAtlas *atlas_ = nullptr;
	MaterialAtlas *materials_ = nullptr;
	IslandAtlas *islands_ = nullptr;
	IslandCullPass *island_cull_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *gen_pass_ = nullptr;
	RaymarchPass *raymarch_pass_ = nullptr;
	CompositePass *composite_pass_ = nullptr;
	DeferredPass *deferred_pass_ = nullptr;
	SunShadowPass *sun_shadow_pass_ = nullptr;
	SunUbo *sun_ubo_ = nullptr;
	InjectPass *inject_pass_ = nullptr;
	LodRasterPass *lod_raster_pass_ = nullptr;
	LodCullPass *lod_cull_pass_ = nullptr;
	HizPass *hiz_pass_ = nullptr;
	GBuffer *gbuffer_ = nullptr;
	CameraUbo *beauty_camera_ = nullptr;
	ContactShadowPass *contact_shadow_pass_ = nullptr;
	SsgiPass *ssgi_pass_ = nullptr;
	SsaoPass *ssao_pass_ = nullptr;
	SsrPass *ssr_pass_ = nullptr;
	OutlinePass *outline_pass_ = nullptr;
	GpuTimings gpu_timings_;
	float prev_view_proj_[16] = {};
	bool has_history_ = false;
	uint32_t beauty_frame_ = 0;
	int normal_roughness_state_ = -1;
	RID downsample_shader_, downsample_pipeline_, downsample_sampler_, downsample_uset_;
	RID downsample_src_, downsample_dst_;
	// --- compositor admission/lifetime state (Task 13, member-for-member from
	// VoxelWorld; the cv wait/signal sites live in try/end_render_callback and
	// shutdown_render_resources[_on_render_thread] above) ---
	mutable std::mutex render_lifetime_mutex_;
	std::condition_variable render_lifetime_cv_;
	bool render_shutting_down_ = false;
	bool render_teardown_deferred_ = false;
	int render_callbacks_ = 0;
	std::condition_variable gpu_teardown_cv_;
	bool gpu_teardown_done_ = false;
	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
	// --- shader hot reload (spec §8), member-for-member from VoxelWorld (Task 14);
	// the latch/mutex acquisition sites live in request/pump_shader_reload above ---
	std::atomic<bool> reload_requested_{false};
	mutable std::mutex reload_mutex_; // mutable: const debug-facade snapshots take it
	int reload_count_ = 0;
	bool reload_last_ok_ = true;
	String reload_last_error_;
	// --- M6 beautification settings (member-for-member from VoxelWorld, Task 14);
	// guarded by beauty_mutex_ per the class contract documented above ---
	mutable std::mutex beauty_mutex_;
	int quality_tier_ = static_cast<int>(ve::QualityTier::kHigh);
	ve::BeautySettings beauty_ = ve::settings_for_tier(ve::QualityTier::kHigh);
};

// Compositor callbacks can outlive the SceneTree during SceneTree::quit(). Admission
// serializes the enabled check, SceneTree/world lookup, and per-world callback guard.
// (Declarations live beside the orchestrator because the per-world half of the state
// moved there in Task 13.)
class VoxelWorld;
bool voxel_try_begin_compositor_callback(const NodePath &world_path, VoxelWorld **world);
void voxel_compositor_callbacks_ready(RenderOrchestrator *render);
void voxel_compositor_callbacks_shutdown_started(RenderOrchestrator *render);

} // namespace godot
