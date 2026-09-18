#pragma once
// Every push-constant and uniform-buffer layout a C++ pass fills and a shader reads: one
// trivially copyable struct per block and a field table beside it. blocks_glsl() writes
// shaders/generated/blocks.glslh from kBlocks; extension/tests/test_gpu_layout.cpp proves each
// table against offsetof and sizeof, so a struct and its shader cannot disagree.
//
// Comments on fields are the only documentation of what a slot carries; the generated GLSL
// has no comments of its own.
#include "gpu_layout/layout.h"
#include "grass/grass_layout.h"
#include "render/camera_params.h"
#include "shade/sun_cascades.h"
#include <cstdint>
#include <string>

namespace ve {

struct SsaoPush {
	int32_t dims[4];  // xy = target size, z = march steps per direction, w = sweep directions
	float params[4];  // x = world-space radius, y = strength, zw = unused
};

struct ContactShadowPush {
	int32_t dims[4];  // xy = target size, z = mode (0 march, 1 apply), w = steps
	float params[4];  // x = reach, y = strength, z = surface bias/hit thickness (metres)
};

struct OutlinePush {
	int32_t dims[4];  // xy = full size, z = have normal-roughness
	float params[4];  // relative depth threshold, normal threshold, darken, unused
};

struct SsrTracePush {
	int32_t dims[4];  // xy = half size, z = steps, w = have normal-roughness
	float params[4];  // reach, start bias, thickness (metres), strength
};

struct SsrApplyPush {
	int32_t dims[4];  // xy = full size
};

struct SsgiPush {
	float prev_view_proj[16];
	int32_t dims[4];    // xy = target size, z = taps, w = have history
	float params[4];    // x = bounce radius (m), y = temporal history weight, z = bounce strength
	float emissive[4];  // x = emissive radius (m), y = emissive strength, zw unused
	int32_t stage[4];   // x = 0 gather into out_raw, 1 resolve out_raw into out_ssgi
};

struct HizPush {
	int32_t dims[4];   // xy = destination size, zw = source size
	int32_t flags[4];  // x = 1 when the source is the scene depth (level 0), else 0
};

struct DeferredPush {
	float inv_view_proj[16];
	float cam[4];       // xyz = camera position
	float sky[4];       // xyz = ambient
	uint32_t flags[4];  // x = beauty flags, y = probe mode
};

struct SunCascadeBlock {
	float view_proj[kSunCascades][16];
	// Per cascade: x = one shadow texel in world metres, y = light-space depth range in the same
	// metres; zw on cascade 0 carries the LoD fade band (fade_start, fade_end).
	float params[kSunCascades][4];
	float splits[4];  // xyz = the cascade radii; w = the count in use (1 when the radius collapsed)
};

struct BeautyCamBlock {
	float view_proj[16];
	float inv_view_proj[16];
	float cam[4];     // xyz = camera position, w = unused
	float screen[4];  // xy = full-resolution size, zw = 1 / size
};

struct SunLightBlock {
	float dir[4];  // xyz = normalized, TOWARD the sun; w unused
	float rgb[4];  // xyz = linear colour * energy; w unused
};

struct LodRasterPush {
	float view_proj[16];
	float cam[4];   // xyz = camera position, w = fade start
	float fade[4];  // x = fade end, yzw unused
};

struct CompositePush {
	float view_proj[16];
	float cam[4];         // xyz = camera position, w = fade start
	float fade[4];        // x = fade end, yzw = camera forward
	float right_tanx[4];  // xyz = camera right, w = tan(fov_x / 2)
	float up_tany[4];     // xyz = camera up,    w = tan(fov_y / 2)
};

struct GrassRasterPush {
	float view_proj[16];
	float cam[4];  // xyz = camera position, w unused
};

struct SunShadowPush {
	float sun_view_proj[16];
};

struct EditsBlock {
	float center[4];  // xyz = brush centre
	float params[4];  // x = radius, y = type, z = material, w = 1 while a brush is shown
};

struct LodCullPush {
	float view_proj[16];
	int32_t params[4];  // x = page count, y = hiz size, z = hiz mips, w = unused
};

struct GrassRegionBlock {
	int32_t dims[4];
	int32_t region_origin[4];
	int32_t atlas_bricks[4];
};

struct DownsamplePush {
	int32_t dims[4];  // xy = destination (half) size
};

struct BrickGenPush {
	int32_t atlas_bricks[4];
};

struct ConsolidatePush {
	int32_t params[4];  // x = brick count, y = region slot, z = op count, w = region table
};

struct BrickMarkPush {
	int32_t region[4];  // xyz = global region coord (may be negative), w = region slot
	int32_t lo[4];      // inclusive global brick coord of the range to scan
	int32_t hi[4];      // inclusive
	int32_t cfg[4];     // x = op count, y = phase (0 release, 1 allocate), z = max jobs, w = force
};

struct RegionFreePush {
	int32_t cfg[4];  // x = region slot
};

struct DispatchArgsPush {
	int32_t pad[4];
};

struct IslandExtractPush {
	float origin_voxel[4];  // xyz = origin, w = voxel size
	int32_t params[4];      // x = dim, y = op count, z = box count, w = override table
};

struct MeshPush {
	int32_t chunk[4];          // xyz = chunk coordinates (job identity only), w = job index
	int32_t params[4];         // x = op count, y = max verts per job, z = max tris per job, w = lattice dim
	float grid[4];             // xyz = the chunk's world origin, w = cell size in metres
	int32_t override_data[4];  // x = override table, y = region slot
};

struct LodBuildPush {
	int32_t job[4];            // xyz = chunk coordinates, w = job index in this batch
	int32_t params[4];         // x = op count, y = max quads per job, z = level, w = unused
	float grid[4];             // xyz = the chunk's world origin, w = the level's cell size
	int32_t override_data[4];  // x = override table, y = region slot
};

} // namespace ve

namespace ve::layout {

inline constexpr Field kSsaoPushFields[] = {
	VE_LAYOUT_FIELD(SsaoPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(SsaoPush, params, Vec4, 0),
};
inline constexpr Field kContactShadowPushFields[] = {
	VE_LAYOUT_FIELD(ContactShadowPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(ContactShadowPush, params, Vec4, 0),
};
inline constexpr Field kOutlinePushFields[] = {
	VE_LAYOUT_FIELD(OutlinePush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(OutlinePush, params, Vec4, 0),
};
inline constexpr Field kSsrTracePushFields[] = {
	VE_LAYOUT_FIELD(SsrTracePush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(SsrTracePush, params, Vec4, 0),
};
inline constexpr Field kSsrApplyPushFields[] = {
	VE_LAYOUT_FIELD(SsrApplyPush, dims, IVec4, 0),
};
inline constexpr Field kSsgiPushFields[] = {
	VE_LAYOUT_FIELD(SsgiPush, prev_view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(SsgiPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(SsgiPush, params, Vec4, 0),
	VE_LAYOUT_FIELD(SsgiPush, emissive, Vec4, 0),
	VE_LAYOUT_FIELD(SsgiPush, stage, IVec4, 0),
};
inline constexpr Field kHizPushFields[] = {
	VE_LAYOUT_FIELD(HizPush, dims, IVec4, 0),
	VE_LAYOUT_FIELD(HizPush, flags, IVec4, 0),
};
inline constexpr Field kDeferredPushFields[] = {
	VE_LAYOUT_FIELD(DeferredPush, inv_view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(DeferredPush, cam, Vec4, 0),
	VE_LAYOUT_FIELD(DeferredPush, sky, Vec4, 0),
	VE_LAYOUT_FIELD(DeferredPush, flags, UVec4, 0),
};
inline constexpr Field kSunCascadeBlockFields[] = {
	VE_LAYOUT_FIELD(SunCascadeBlock, view_proj, Mat4, kSunCascades),
	VE_LAYOUT_FIELD(SunCascadeBlock, params, Vec4, kSunCascades),
	VE_LAYOUT_FIELD(SunCascadeBlock, splits, Vec4, 0),
};
inline constexpr Field kBeautyCamBlockFields[] = {
	VE_LAYOUT_FIELD(BeautyCamBlock, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(BeautyCamBlock, inv_view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(BeautyCamBlock, cam, Vec4, 0),
	VE_LAYOUT_FIELD(BeautyCamBlock, screen, Vec4, 0),
};
inline constexpr Field kSunLightBlockFields[] = {
	VE_LAYOUT_FIELD(SunLightBlock, dir, Vec4, 0),
	VE_LAYOUT_FIELD(SunLightBlock, rgb, Vec4, 0),
};
inline constexpr Field kLodRasterPushFields[] = {
	VE_LAYOUT_FIELD(LodRasterPush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(LodRasterPush, cam, Vec4, 0),
	VE_LAYOUT_FIELD(LodRasterPush, fade, Vec4, 0),
};
inline constexpr Field kCompositePushFields[] = {
	VE_LAYOUT_FIELD(CompositePush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(CompositePush, cam, Vec4, 0),
	VE_LAYOUT_FIELD(CompositePush, fade, Vec4, 0),
	VE_LAYOUT_FIELD(CompositePush, right_tanx, Vec4, 0),
	VE_LAYOUT_FIELD(CompositePush, up_tany, Vec4, 0),
};
inline constexpr Field kGrassRasterPushFields[] = {
	VE_LAYOUT_FIELD(GrassRasterPush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(GrassRasterPush, cam, Vec4, 0),
};
inline constexpr Field kSunShadowPushFields[] = {
	VE_LAYOUT_FIELD(SunShadowPush, sun_view_proj, Mat4, 0),
};
inline constexpr Field kCameraParamsFields[] = {
	VE_LAYOUT_FIELD(CameraParams, cam_pos, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, cam_right, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, cam_up, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, cam_fwd, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, params, Vec4, 0),
	VE_LAYOUT_FIELD(CameraParams, dims, IVec4, 0),
	VE_LAYOUT_FIELD(CameraParams, region_origin, IVec4, 0),
	VE_LAYOUT_FIELD(CameraParams, atlas_bricks, IVec4, 0),
};
inline constexpr Field kEditsBlockFields[] = {
	VE_LAYOUT_FIELD(EditsBlock, center, Vec4, 0),
	VE_LAYOUT_FIELD(EditsBlock, params, Vec4, 0),
};
inline constexpr Field kLodCullPushFields[] = {
	VE_LAYOUT_FIELD(LodCullPush, view_proj, Mat4, 0),
	VE_LAYOUT_FIELD(LodCullPush, params, IVec4, 0),
};
inline constexpr Field kGrassParamsFields[] = {
	VE_LAYOUT_FIELD(GrassParams, cam, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, planes, Vec4, 6),
	VE_LAYOUT_FIELD(GrassParams, brick_min, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, brick_dim, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, ring_end, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, ring_blades, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, blade, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, wind, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, style, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, shape, Vec4, 0),
	VE_LAYOUT_FIELD(GrassParams, limits, IVec4, 0),
	VE_LAYOUT_FIELD(GrassParams, far, IVec4, 0),
};
inline constexpr Field kGrassRegionBlockFields[] = {
	VE_LAYOUT_FIELD(GrassRegionBlock, dims, IVec4, 0),
	VE_LAYOUT_FIELD(GrassRegionBlock, region_origin, IVec4, 0),
	VE_LAYOUT_FIELD(GrassRegionBlock, atlas_bricks, IVec4, 0),
};
inline constexpr Field kDownsamplePushFields[] = {
	VE_LAYOUT_FIELD(DownsamplePush, dims, IVec4, 0),
};
inline constexpr Field kBrickGenPushFields[] = {
	VE_LAYOUT_FIELD(BrickGenPush, atlas_bricks, IVec4, 0),
};
inline constexpr Field kConsolidatePushFields[] = {
	VE_LAYOUT_FIELD(ConsolidatePush, params, IVec4, 0),
};
inline constexpr Field kBrickMarkPushFields[] = {
	VE_LAYOUT_FIELD(BrickMarkPush, region, IVec4, 0),
	VE_LAYOUT_FIELD(BrickMarkPush, lo, IVec4, 0),
	VE_LAYOUT_FIELD(BrickMarkPush, hi, IVec4, 0),
	VE_LAYOUT_FIELD(BrickMarkPush, cfg, IVec4, 0),
};
inline constexpr Field kRegionFreePushFields[] = {
	VE_LAYOUT_FIELD(RegionFreePush, cfg, IVec4, 0),
};
inline constexpr Field kDispatchArgsPushFields[] = {
	VE_LAYOUT_FIELD(DispatchArgsPush, pad, IVec4, 0),
};
inline constexpr Field kIslandExtractPushFields[] = {
	VE_LAYOUT_FIELD(IslandExtractPush, origin_voxel, Vec4, 0),
	VE_LAYOUT_FIELD(IslandExtractPush, params, IVec4, 0),
};
inline constexpr Field kMeshPushFields[] = {
	VE_LAYOUT_FIELD(MeshPush, chunk, IVec4, 0),
	VE_LAYOUT_FIELD(MeshPush, params, IVec4, 0),
	VE_LAYOUT_FIELD(MeshPush, grid, Vec4, 0),
	VE_LAYOUT_FIELD(MeshPush, override_data, IVec4, 0),
};
inline constexpr Field kLodBuildPushFields[] = {
	VE_LAYOUT_FIELD(LodBuildPush, job, IVec4, 0),
	VE_LAYOUT_FIELD(LodBuildPush, params, IVec4, 0),
	VE_LAYOUT_FIELD(LodBuildPush, grid, Vec4, 0),
	VE_LAYOUT_FIELD(LodBuildPush, override_data, IVec4, 0),
};

inline constexpr Block kBlocks[] = {
	VE_LAYOUT_BLOCK(SsaoPush, "SSAO_PUSH_FIELDS", kSsaoPushFields),
	VE_LAYOUT_BLOCK(ContactShadowPush, "CONTACT_SHADOW_PUSH_FIELDS", kContactShadowPushFields),
	VE_LAYOUT_BLOCK(OutlinePush, "OUTLINE_PUSH_FIELDS", kOutlinePushFields),
	VE_LAYOUT_BLOCK(SsrTracePush, "SSR_TRACE_PUSH_FIELDS", kSsrTracePushFields),
	VE_LAYOUT_BLOCK(SsrApplyPush, "SSR_APPLY_PUSH_FIELDS", kSsrApplyPushFields),
	VE_LAYOUT_BLOCK(SsgiPush, "SSGI_PUSH_FIELDS", kSsgiPushFields),
	VE_LAYOUT_BLOCK(HizPush, "HIZ_PUSH_FIELDS", kHizPushFields),
	VE_LAYOUT_BLOCK(DeferredPush, "DEFERRED_PUSH_FIELDS", kDeferredPushFields),
	VE_LAYOUT_BLOCK(SunCascadeBlock, "SUN_CASCADE_BLOCK_FIELDS", kSunCascadeBlockFields),
	VE_LAYOUT_BLOCK(BeautyCamBlock, "BEAUTY_CAM_FIELDS", kBeautyCamBlockFields),
	VE_LAYOUT_BLOCK(SunLightBlock, "SUN_LIGHT_FIELDS", kSunLightBlockFields),
	VE_LAYOUT_BLOCK(LodRasterPush, "LOD_RASTER_PUSH_FIELDS", kLodRasterPushFields),
	VE_LAYOUT_BLOCK(CompositePush, "COMPOSITE_PUSH_FIELDS", kCompositePushFields),
	VE_LAYOUT_BLOCK(GrassRasterPush, "GRASS_RASTER_PUSH_FIELDS", kGrassRasterPushFields),
	VE_LAYOUT_BLOCK(SunShadowPush, "SUN_SHADOW_PUSH_FIELDS", kSunShadowPushFields),
	VE_LAYOUT_BLOCK(CameraParams, "CAMERA_PARAMS_FIELDS", kCameraParamsFields),
	VE_LAYOUT_BLOCK(EditsBlock, "EDITS_BLOCK_FIELDS", kEditsBlockFields),
	VE_LAYOUT_BLOCK(LodCullPush, "LOD_CULL_PUSH_FIELDS", kLodCullPushFields),
	VE_LAYOUT_BLOCK(GrassParams, "GRASS_PARAMS_FIELDS", kGrassParamsFields),
	VE_LAYOUT_BLOCK(GrassRegionBlock, "GRASS_REGION_FIELDS", kGrassRegionBlockFields),
	VE_LAYOUT_BLOCK(DownsamplePush, "DOWNSAMPLE_PUSH_FIELDS", kDownsamplePushFields),
	VE_LAYOUT_BLOCK(BrickGenPush, "BRICK_GEN_PUSH_FIELDS", kBrickGenPushFields),
	VE_LAYOUT_BLOCK(ConsolidatePush, "CONSOLIDATE_PUSH_FIELDS", kConsolidatePushFields),
	VE_LAYOUT_BLOCK(BrickMarkPush, "BRICK_MARK_PUSH_FIELDS", kBrickMarkPushFields),
	VE_LAYOUT_BLOCK(RegionFreePush, "REGION_FREE_PUSH_FIELDS", kRegionFreePushFields),
	VE_LAYOUT_BLOCK(DispatchArgsPush, "DISPATCH_ARGS_PUSH_FIELDS", kDispatchArgsPushFields),
	VE_LAYOUT_BLOCK(IslandExtractPush, "ISLAND_EXTRACT_PUSH_FIELDS", kIslandExtractPushFields),
	VE_LAYOUT_BLOCK(MeshPush, "MESH_PUSH_FIELDS", kMeshPushFields),
	VE_LAYOUT_BLOCK(LodBuildPush, "LOD_BUILD_PUSH_FIELDS", kLodBuildPushFields),
};

// The exact contents of shaders/generated/blocks.glslh.
std::string blocks_glsl();

} // namespace ve::layout
