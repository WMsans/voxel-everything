#pragma once
// RenderOrchestrator — the ~30 pass pointers, the downsample pipeline, and the
// RenderingDevice ownership extracted from VoxelWorld (spec Phase 4a / Task 12).
// Needs GPU/Godot runtime; NOT in the native-test pure_sources (src/render/*.cpp
// is never globbed there -- see SConstruct).
//
// Construction/teardown ORDER of the GPU objects below is load-bearing (the
// CPU-outlives-GPU invariant): ensure_gpu_graph() and the teardown_*() halves
// preserve the exact allocation/deallocation sequence VoxelWorld used before
// the split. Lifetime/admission moves in Phase 4b and reload/beauty in Phase
// 4c; until then VoxelWorld drives those paths THROUGH this class.
//
// No VoxelWorld* lives here: collaborators arrive as injected handles/addresses
// (same rule as ConsolidationCoordinator's Collaborators -- the store/spine
// objects are created lazily and destroyed across teardown cycles, so their
// addresses are re-read at every use instead of caching stranded pointers).
#include <godot_cpp/variant/rid.hpp>

#include "render/gpu_timings.h"
#include "world/region.h"

namespace godot {

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
class ContactShadowPass;
class SsgiPass;
class SsrPass;
class OutlinePass;

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
	};

	explicit RenderOrchestrator(Collaborators handles);

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
	ContactShadowPass *contact_shadow_pass() { return contact_shadow_pass_; }
	SsgiPass *ssgi_pass() { return ssgi_pass_; }
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
	GpuTimings gpu_timings_;
	float prev_view_proj_[16] = {};
	bool has_history_ = false;
	uint32_t beauty_frame_ = 0;
	int normal_roughness_state_ = -1;
	RID downsample_shader_, downsample_pipeline_, downsample_sampler_, downsample_uset_;
	RID downsample_src_, downsample_dst_;
	RenderingDevice *main_rd_ = nullptr;
	RenderingDevice *local_rd_ = nullptr; // owned when use_local_device_
};

} // namespace godot
