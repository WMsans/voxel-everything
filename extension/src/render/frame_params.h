#pragma once
// Pure inputs shared by VoxelFrame and the pass-level debug probes. No godot-cpp: this file is
// in the native test build (SConstruct). Before it existed, hooks.cpp rebuilt these by hand in
// ~15 places and the copies drifted from the shipped frame (spec 2026-09-13 §1).
#include <cstdint>
#include "lod/lod_tree.h"
#include "render/camera_params.h"
#include "world/region.h"
#include "world/region_window.h"

namespace ve {

// A synthetic perspective camera stated as position + forward. `lod` is the reverse-Z
// view_proj the rasters use; `right/up/fwd` and the tangents are what the raymarcher uses.
// Both come from the same up hint, so they agree on the pixel grid.
struct ProbeCamera {
	LodCamera lod;
	float right[3] = {};
	float up[3] = {};
	float fwd[3] = {};
	float tan_x = 0.0f;
	float tan_y = 0.0f;
};

// World up, unless the view is within ~25 degrees of vertical, where +Z is used instead.
void probe_up_hint(const float fwd[3], float out_up[3]);

ProbeCamera probe_camera(const float pos[3], const float fwd[3], int w, int h,
		float fov_y_rad, float z_near, float z_far);

// The region window, live island slot count and atlas grid -- the fields of the raymarch push
// block that describe the WORLD rather than the camera. Leaves params, cam_pos.w,
// region_origin.w and atlas_bricks.w (island cull tiles) untouched.
void set_near_field_world(CameraParams *cp, const RegionWindow &win, int island_slots,
		IVec3 atlas_bricks);

// The beauty flag bits ride in cam_pos.w as raw bits (raymarch.comp.glsl reads them back with
// floatBitsToUint).
void set_near_field_flags(CameraParams *cp, uint32_t beauty_flags);

} // namespace ve
