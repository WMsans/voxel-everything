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
#include <functional>
#include <mutex>
#include <vector>

#include "render/gpu/gpu.h"
#include "grass/grass_settings_store.h"
#include "render/frame.h"
#include "render/gpu_timings.h"
#include "render/island_handoff.h"
#include "shade/beauty_settings.h"
#include "shade/beauty_settings_store.h"
#include "settings/render_settings.h"
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
class LodSystem;
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
class FieldContextSet;
class ContactShadowPass;
class SsgiPass;
class SsaoPass;
class SsrPass;
class OutlinePass;
class GrassScatterPass;
class GrassRasterPass;
class Object;

// Every GPU object of the pass graph. The orchestrator creates them in ensure_gpu_graph() and
// deletes them in teardown_gpu()'s halves, in the load-bearing orders documented there; a null
// field means "not built" (never initialized, failed soft, or torn down). VoxelFrame, LodSystem
// and the debug facade's pass probes read it; nobody else creates or deletes these objects.
struct RenderPasses {
	GpuAtlas *atlas = nullptr;
	MaterialAtlas *materials = nullptr;
	IslandAtlas *islands = nullptr;
	IslandCullPass *island_cull = nullptr;
	RegionPass *region = nullptr;
	BrickGenPass *gen = nullptr;
	RaymarchPass *raymarch = nullptr;
	CompositePass *composite = nullptr;
	DeferredPass *deferred = nullptr;
	SunShadowPass *sun_shadow = nullptr;
	SunUbo *sun_ubo = nullptr;
	FieldContextSet *field_context = nullptr;
	InjectPass *inject = nullptr;
	LodRasterPass *lod_raster = nullptr;
	LodCullPass *lod_cull = nullptr;
	HizPass *hiz = nullptr;
	GBuffer *gbuffer = nullptr;
	CameraUbo *beauty_camera = nullptr;
	ContactShadowPass *contact_shadow = nullptr;
	SsgiPass *ssgi = nullptr;
	SsaoPass *ssao = nullptr;
	SsrPass *ssr = nullptr;
	OutlinePass *outline = nullptr;
	GrassScatterPass *grass_scatter = nullptr;
	GrassRasterPass *grass_raster = nullptr;
};

class RenderOrchestrator {
public:
	struct Collaborators {
		// Device-selection seam: use_local_device_ stays a VoxelWorld property (ClassDB).
		const bool *use_local_device = nullptr;
		// Config/residency spine steps sit mid-sequence in ensure_gpu_graph().
		WorldStore *store = nullptr;
		// teardown_gpu() calls lod->release_gpu() where the LoD statements always sat.
		LodSystem *lod = nullptr;
		// Callable target for the queued render-thread teardown (see class comment).
		Object *callback_owner = nullptr;
		// pump_shader_reload()'s re-init arm: VoxelWorld::ensure_initialized().
		std::function<void()> ensure_initialized;
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
	// global admission lock (admission then render lifetime, unchanged from the pre-move bodies;
	// see core/edit_pipeline.h).
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
	void set_effect_value(const String &name, float value);
	float get_effect_value(const String &name) const;
	bool get_effect_enabled(const String &name) const;
	// Returns an immutable value snapshot. Render callbacks must take this once per frame
	// and pass the copy through their work; the mutex is never held during render work.
	ve::BeautySettings beauty_settings() const;
	// Settings + tier together, for debug_beauty_settings.
	void beauty_snapshot(ve::BeautySettings *out_settings, int *out_tier) const;
	// The three stores by group name ("render", "beauty", "grass"); nullptr otherwise.
	// VoxelSettings addresses them through this. Main thread.
	ve::SettingsGroup *settings_group(const char *name);

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

	// --- island handoff (spec 2026-09-14 §3.2; moved from VoxelWorld) ---
	IslandHandoff &handoff() { return handoff_; }
	// High-water mark for the raymarcher. Render thread; lock-free (two atomics).
	int island_slot_count() const {
		return handoff_.slot_count(islands_enabled_.load(std::memory_order_relaxed));
	}
	// Render thread, before the streamer runs. Returns how many uploads landed.
	int drain_island_uploads(RenderingDevice *device);

	// --- render lifetime state and per-frame knobs (moved from VoxelWorld, spec 2026-09-14
	// §3.1). Guards unchanged: plain fields stay plain, atomics stay atomic, the sun keeps
	// its own mutex. ---
	bool initialized() const { return initialized_; }
	void mark_initialized() { initialized_ = true; }
	WorldStreamer *streamer() const { return streamer_; }
	WorldStreamer **streamer_slot() { return &streamer_; } // ConsolidationCoordinator wiring
	// Debug-settable compact-normal budget; 0 = GpuAtlasConfig's default. Set before init.
	void set_normal_pool_bytes(uint32_t bytes) { normal_pool_bytes_ = bytes; }
	bool last_hiz_readback_was_pending() const { return last_hiz_readback_was_pending_; }
	bool last_hiz_readback_was_drained() const { return last_hiz_readback_was_drained_; }
	void set_sun_state(const ve::SunState &sun);
	ve::SunState sun_state() const;
	void set_near_field_scale(float v); // clamps to [0.1, 1]
	float near_field_scale() const { return near_field_scale_.load(std::memory_order_relaxed); }
	bool near_field_enabled() const { return near_field_enabled_.load(std::memory_order_relaxed); }
	void set_sun_cascade_min_level(bool v) { sun_cascade_min_level_ = v; }
	bool sun_cascade_min_level() const { return sun_cascade_min_level_; }
	// Everything VoxelFrame samples once per frame.
	FrameSettings frame_settings() const;
	// The one ordered run of every voxel render stage (render/frame.h). Compositors and the
	// headless debug probes call it.
	VoxelFrame &frame() { return frame_; }

	// Address-of slots for collaborators (ConsolidationCoordinator wiring) that
	// re-read lazily-created objects at every use.
	GpuAtlas **atlas_slot() { return &passes_.atlas; }
	RenderingDevice **main_rd_slot() { return &main_rd_; }
	RenderingDevice **local_rd_slot() { return &local_rd_; }

	const RenderPasses &passes() const { return passes_; }
	ve::GrassSettings grass_settings() const { return grass_settings_.get(); }
	bool set_grass_value(const char *n, float v) { return grass_settings_.set_value(n, v); }
	float grass_value(const char *n) const { return grass_settings_.value(n); }
	GpuTimings *gpu_timings() { return &gpu_timings_; }

	// --- history/beauty frame state (moved with the pass graph) ---
	const float *prev_view_proj() const { return prev_view_proj_; }
	// True only while the history texture the last downsample wrote into is STILL the one the
	// G-buffer hands out. A viewport reconfigure -- which a runtime render-scale change is --
	// makes the engine drop the voxel_gbuf context, and GBuffer::ensure() recreates `history`
	// with undefined contents. The latch describes a texture, not an epoch, so it has to fall
	// with the texture it described; otherwise SSGI bounces uninitialised memory into a
	// temporal accumulator whose neighbourhood clamp then spreads it a texel per frame.
	bool has_history() const;
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
	// island_cull then islands (between clear_residency() and the atlas delete).
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
	// Debug-only: the label of every step teardown_gpu() ran, in order, for its most recent
	// run. The render lifetime contract pins this sequence (spec 2026-09-14 §5.1); a change in
	// it means the deallocation order changed.
	const std::vector<const char *> &teardown_trace() const { return teardown_trace_; }
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
	Collaborators handles_;
	std::vector<const char *> teardown_trace_;

	RenderPasses passes_;
	// Grass knobs live here, separate from BeautySettings (design doc section 7).
	ve::GrassSettingsStore grass_settings_;
	GpuTimings gpu_timings_;
	IslandHandoff handoff_;
	WorldStreamer *streamer_ = nullptr; // created inside ensure_gpu_graph(), deleted in teardown_gpu()
	bool initialized_ = false;
	uint32_t normal_pool_bytes_ = 0;
	bool last_hiz_readback_was_pending_ = false;
	bool last_hiz_readback_was_drained_ = true;
	std::atomic<bool> islands_enabled_{true};
	std::atomic<bool> near_field_enabled_{true};
	std::atomic<float> near_field_scale_{0.66f};
	bool sun_cascade_min_level_ = true;
	mutable std::mutex sun_mutex_;
	ve::SunState sun_state_;
	float prev_view_proj_[16] = {};
	bool has_history_ = false;
	// The history texture has_history_ refers to; see has_history().
	RID history_texture_;
	uint32_t beauty_frame_ = 0;
	int normal_roughness_state_ = -1;
	gpu::Group downsample_group_;
	gpu::Program downsample_;
	RID downsample_sampler_;
	gpu::SetCache downsample_set_;
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
	// Setters run on the main thread; render callbacks take value snapshots through
	// beauty_settings(). The store's mutex is never held during render work.
	std::atomic<int> quality_tier_{static_cast<int>(ve::QualityTier::kHigh)};
	// Source of truth for the budget dials. Its listener mirrors near_field_scale, near_field and
	// islands into the orchestrator's atomics (the render thread's lock-free reads) and rebases
	// beauty_ when the tier moves. The listener is attached in the constructor body, after every
	// member it touches exists.
	ve::RenderSettingsStore render_settings_;
	static void on_render_resolved(const ve::RenderSettings &s, void *ctx);
	ve::BeautySettingsStore beauty_;
	VoxelFrame frame_;
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
