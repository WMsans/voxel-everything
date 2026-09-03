#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"

namespace godot {

// Workgroup shape of raymarch.comp.glsl. MUST match its layout(local_size_*): the dispatch
// divides the target size by these, and before they existed that divisor was hard-coded to 8
// while the shader was free to declare any layout -- a mismatch renders a fraction of the
// target and reads as a large speed-up.
//
// Measured on an Apple M1 at 2450x1319 with the near field at half scale, the square tile is
// what this workload wants: 8x8 = 18.33 ms/frame, 8x4 = 18.62, 16x8 = 18.57, 16x4 = 18.90,
// 32x2 = 20.21, 64x1 = 20.73. Wide rows were worth testing -- ray length varies far more down
// the screen than across it, so a one-row group holds a SIMD's rays at nearly the same depth
// instead of pairing a short ray with a long one -- and lost anyway.
inline constexpr int kRaymarchGroupX = 8;
inline constexpr int kRaymarchGroupY = 8;

class GpuAtlas;
class IslandAtlas;
class MaterialAtlas;
class FieldContextSet;

class RaymarchPass {
public:
	~RaymarchPass(); // calls teardown(): frees RIDs on rd_ (device must be alive)
	void initialize(RenderingDevice *rd);
	void teardown();
	// The shared material arrays are owned by VoxelWorld's MaterialAtlas; RaymarchPass only
	// mirrors their RIDs into its uniform set. Call once after initialize() and before the
	// first render (VoxelWorld does this).
	void set_materials(const MaterialAtlas &materials);
	// The sun UBO is owned by RenderOrchestrator; this pass only mirrors its RID into the
	// uniform set. Call once after initialize() and before the first render.
	void set_sun_ubo(RID buffer);
	// `islands` may be null (no island support yet initialised) and `tile_mask` invalid (no
	// cull pass has run); both fall back to the atlas's own all-ones single-entry mask.
	// field_context is the orchestrator's set 1 (may be null when its build failed).
	bool render(RenderingDevice *rd, const GpuAtlas &atlas, const IslandAtlas *islands,
			RID tile_mask, const ve::CameraParams &cam, int width, int height,
			const float edit_state[6], const FieldContextSet *field_context);
	bool targets_need_rebuild(int width, int height, RID mask) const;

	RID albedo_texture() const { return albedo_; }
	RID surface_texture() const { return surface_; }
	RID hitpos_texture() const { return hitpos_; }
	RID cost_buffer() const { return cost_buf_; }

private:
	RID make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h);
	void rebuild_targets(RenderingDevice *rd, const GpuAtlas &atlas, const IslandAtlas *islands,
			RID tile_mask, int w, int h);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_;
	RID sampler_;     // shared NEAREST sampler, created once
	// The SDF atlas is the one target sampled with hardware trilinear (binding 2): its
	// 17-voxel apron keeps a filtered fetch inside the brick's own block, so one fetch
	// replaces the eight brick_sdf() used to issue. The material and min-max atlases stay
	// on sampler_ -- they are integer textures and cannot be filtered at all.
	RID sampler_linear_;
	RID edits_ubo_;   // 32-byte uniform buffer, updated every render
	RID sun_ubo_; // NOT owned: RenderOrchestrator frees it
	RID sun_uset_; // set 2 uniform set; NOT owned: RenderOrchestrator frees the buffer
	RID material_albedo_, material_surface_, material_sampler_;
	RID albedo_, surface_, hitpos_, cost_buf_, uset_, uset_mask_;
	int width_ = 0, height_ = 0;
};

} // namespace godot
