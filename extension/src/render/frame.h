#pragma once
// VoxelFrame -- one ordered run of every voxel render stage for one camera (CONTEXT.md:
// "Frame"). Owns the stage ORDER and the per-frame input packing; passes own their GPU
// programs and know nothing about the order. Both compositors and the headless debug probes
// call this, so a gdUnit probe exercises the path that ships
// (docs/superpowers/specs/2026-09-13-frame-module-design.md).
//
// Needs RenderingDevice: not in the native test build. The pure input builders it shares
// with pass tests live in render/frame_params.h.
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>
#include <mutex>
#include "render/headless_targets.h"
#include "conifers/conifer_layout.h"
#include "grass/grass_layout.h"
#include "leaves/leaf_layout.h"
#include "shade/sun_ortho.h"
#include "shade/sun_state.h"

namespace godot {

class LodSystem;
class RenderOrchestrator;
class RenderSceneBuffersRD;
class WorldStore;
class GBuffer;
class GpuAtlas;
// Per-frame values RenderOrchestrator owns, sampled once per call.
struct FrameSettings {
	ve::SunState sun;
	float near_field_scale = 0.66f;
	bool near_field_enabled = true;
	bool raster_mode = false;
	bool sun_cascade_min_level = true;
};

// One bit per timing label the frame records. Order is the frame's own order.
enum FrameStage : uint32_t {
	kStageStream,
	kStageRaymarch,
	kStageComposite,
	kStageLod,
	kStageSunShadow,
	kStageGrass,
	kStageSsgi,
	kStageDeferred,
	kStageSsao,
	kStageInject,
	kStageContact,
	kStageSsr,
	kStageOutlines,
	kStageHistory,
	kStageShell,
	kStageTransparency,
	kStageCount
};

// The GPU-timings label for a stage ("stream", "raymarch", ...). Same strings the compositors
// always used, so the benchmark parser is unaffected.
const char *frame_stage_name(FrameStage stage);

// Debug-only knobs that existing probes already had. Every default is the shipped frame; the
// compositors never set these.
struct FrameDebug {
	int deferred_view = 0;        // DeferredPass::Params::probe_mode (debug_deferred_probe)
	bool skip_far_field = false;  // debug_seam_probe(skip_lod): leave the far field out
	RID marker;                   // R8_UINT field-ownership marker (debug_seam_probe)
	Vector2i lod_viewport;        // (0,0) = size. The LoD suites settle the walk at 2560x1440
	                              // through debug_lod_tick; a small probe frame must tick with
	                              // the same viewport or it measures a re-selecting walk.
};

struct FrameInputs {
	Transform3D cam;
	Projection proj;          // the engine's scene projection (y-flipped) or a synthetic one
	Vector2i size;            // internal render size
	RID scene_color;          // written by inject, read and written by the post-opaque stages
	RID scene_depth;
	RID normal_roughness;     // optional; post-opaque only
	RenderSceneBuffersRD *rsb = nullptr; // GBuffer::ensure context; null = owned G-buffer
	FrameDebug debug;
};

// Diagnostics the frame writes about itself. Read through last_frame(), which copies.
struct FrameRecord {
	float fade_start = 0.0f;
	float fade_end = 0.0f;
	bool lod_two_phase = false;
	bool hiz_built = false;
	int lod_first_pass_count = 0;
	uint32_t stages_ok = 0;         // bit (1u << FrameStage): the stage's timing label ended
	uint32_t stages_cancelled = 0;  // bit: the stage started and was cancelled
	bool stage_ok(FrameStage s) const { return (stages_ok & (1u << s)) != 0u; }
};

class VoxelFrame {
public:
	VoxelFrame(RenderOrchestrator &render, LodSystem &lod, WorldStore &store);

	// PRE_OPAQUE half: streaming, near field, far field + sun cascades, grass, SSGI,
	// deferred (with SSAO), inject. False when the frame aborted.
	bool render_pre_opaque(RenderingDevice *rd, const FrameInputs &in);
	// POST_OPAQUE half: contact shadows, SSR, outlines, history, and the GPU-timings frame end
	// that render_pre_opaque began.
	bool render_post_opaque(RenderingDevice *rd, const FrameInputs &in);

	// The synthetic probe camera as engine-shaped inputs: cam and proj satisfy
	// proj * cam.affine_inverse() == ve::probe_camera(...).lod.view_proj, so the frame derives
	// exactly the tangents and basis the probe camera states.
	static FrameInputs looking_at(Vector3 pos, Vector3 fwd, int w, int h,
			float fov_y_rad = 1.0471975512f, float z_near = 0.05f, float z_far = 4000.0f);
	// Local device only. Fills scene_color/scene_depth with frame-owned targets cleared to
	// black / reverse-Z far, and drops pass framebuffers that reference a G-buffer about to be
	// reallocated. On failure the returned scene_color is invalid.
	FrameInputs prepare_headless(RenderingDevice *rd, const FrameInputs &in);
	// prepare_headless + both halves, as the engine would call them. Does NOT submit: the
	// caller submits and syncs before reading anything back.
	bool render_headless(RenderingDevice *rd, const FrameInputs &in);
	// Frees the headless targets. Call before the local device is dropped.
	void release_gpu();

	FrameRecord last_frame() const;

	// The SHIPPING sun fit for one cascade, centred on the last LoD walk's camera.
	ve::SunOrtho sun_ortho(int cascade) const;
	// The SHIPPING grass layout: settings clamped to how far grass can actually be placed.
	ve::GrassLayout grass_layout(const float cam_pos[3], const float view_proj[16]) const;
	// The SHIPPING leaf layout. No reach clamp beside grass's: the stage-1 chop check reads
	// the live atlas, so a canopy can never stand past where trunk voxels are resident --
	// the residency limit enforces itself, and LeafSettings::reach_m is the honest dial.
	ve::LeafLayout leaf_layout(const float cam_pos[3], const float view_proj[16]) const;
	// The SHIPPING conifer layout: the cull/scatter block, plus the raster block the reused
	// LeafRasterPass reads for needle style.
	ve::ConiferLayout conifer_layout(const float cam_pos[3], const float view_proj[16]) const;
	// The conifer block: cull, card scatter, card raster (and Task 7's imposter raster).
	// Called by the compositor and by debug_conifer_stats -- one code path (plan deviation 14).
	// False on failure; the caller cancels the timing marker.
	bool draw_conifers(RenderingDevice *rd, GpuAtlas &atlas, GBuffer &gb,
			const Projection &view_proj, const float cam_pos[3], float time_s, bool raster_mode);

private:
	float grass_reach_limit_m() const;
	void note_fade_band(float fade_start, float fade_end);
	void note_lod_cull(bool two_phase, bool hiz_built, int first_pass_count);
	void end_stage(RenderingDevice *rd, FrameStage stage);
	void cancel_stage(FrameStage stage);
	void reset_stages();
	// Advances scatter_epoch_ while the atlas or the sun is changing: the frame streaming
	// recorded loads/evicts/edit jobs, edit_seq moved, or the sun turned -- and for two
	// frames after, for work that lands a frame late. Grass and leaves skip their scatter
	// while it holds still (GrassScatterPass::run).
	void note_scatter_world(int stream_actions, const ve::SunState &sun);

	HeadlessTargets headless_;
	uint64_t scatter_epoch_ = 0;
	int64_t scatter_edit_seq_ = -1;
	float scatter_sun_[3] = {};
	int scatter_settle_ = 0;

	RenderOrchestrator &render_;
	LodSystem &lod_;
	WorldStore &store_;

	mutable std::mutex record_mutex_;
	FrameRecord record_;
};

} // namespace godot
