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
#include <mutex>
#include "grass/grass_layout.h"
#include "shade/sun_ortho.h"
#include "shade/sun_state.h"
#include "world/region_window.h"

namespace godot {

class LodSystem;
class RenderOrchestrator;
class RenderSceneBuffersRD;
class WorldStore;
class WorldStreamer;

// Per-frame values VoxelWorld still owns, sampled once per call.
struct FrameSettings {
	ve::SunState sun;
	float near_field_scale = 0.66f;
	bool near_field_enabled = true;
	bool sun_cascade_min_level = true;
};

// TEMPORARY seam (spec §4.6): one adapter (VoxelWorld), accepted so the island handoff queue
// and its mutex do not move in this sub-project. Sub-project 2 moves that queue into the
// render lifetime owner and deletes this interface.
class FrameHost {
public:
	virtual int island_slot_count() const = 0;
	virtual int drain_island_uploads(RenderingDevice *rd) = 0;
	virtual WorldStreamer *streamer() = 0;
	virtual FrameSettings frame_settings() const = 0;

protected:
	~FrameHost() = default;
};

struct FrameInputs {
	Transform3D cam;
	Projection proj;          // the engine's scene projection (y-flipped) or a synthetic one
	Vector2i size;            // internal render size
	RID scene_color;          // written by inject, read and written by the post-opaque stages
	RID scene_depth;
	RID normal_roughness;     // optional; post-opaque only
	RenderSceneBuffersRD *rsb = nullptr; // GBuffer::ensure context; null = owned G-buffer
};

// Diagnostics the frame writes about itself. Read through last_frame(), which copies.
struct FrameRecord {
	float fade_start = 0.0f;
	float fade_end = 0.0f;
	bool lod_two_phase = false;
	bool hiz_built = false;
	int lod_first_pass_count = 0;
};

class VoxelFrame {
public:
	VoxelFrame(RenderOrchestrator &render, LodSystem &lod, WorldStore &store, FrameHost &host);

	// PRE_OPAQUE half: streaming, near field, far field + sun cascades, grass, SSGI,
	// deferred (with SSAO), inject. False when the frame aborted.
	bool render_pre_opaque(RenderingDevice *rd, const FrameInputs &in);
	// POST_OPAQUE half: contact shadows, SSR, outlines, history, and the GPU-timings frame end
	// that render_pre_opaque began.
	bool render_post_opaque(RenderingDevice *rd, const FrameInputs &in);

	FrameRecord last_frame() const;

	// The SHIPPING sun fit for one cascade, centred on the last LoD walk's camera.
	ve::SunOrtho sun_ortho(int cascade) const;
	// The SHIPPING grass layout: settings clamped to how far grass can actually be placed.
	ve::GrassLayout grass_layout(const float cam_pos[3], const float view_proj[16]) const;

private:
	ve::RegionWindow region_window() const;
	float grass_reach_limit_m() const;
	void note_fade_band(float fade_start, float fade_end);
	void note_lod_cull(bool two_phase, bool hiz_built, int first_pass_count);

	RenderOrchestrator &render_;
	LodSystem &lod_;
	WorldStore &store_;
	FrameHost &host_;

	mutable std::mutex record_mutex_;
	FrameRecord record_;
};

} // namespace godot
