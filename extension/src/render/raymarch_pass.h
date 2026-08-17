#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"

namespace godot {

class GpuAtlas;
class IslandAtlas;
class MaterialAtlas;

class RaymarchPass {
public:
	~RaymarchPass(); // calls teardown(): frees RIDs on rd_ (device must be alive)
	void initialize(RenderingDevice *rd);
	void teardown();
	// The shared material arrays are owned by VoxelWorld's MaterialAtlas; RaymarchPass only
	// mirrors their RIDs into its uniform set. Call once after initialize() and before the
	// first render (VoxelWorld does this).
	void set_materials(const MaterialAtlas &materials);
	// `islands` may be null (no island support yet initialised) and `tile_mask` invalid (no
	// cull pass has run); both fall back to the atlas's own all-ones single-entry mask.
	bool render(RenderingDevice *rd, const GpuAtlas &atlas, const IslandAtlas *islands,
			RID tile_mask, const ve::CameraParams &cam, int width, int height,
			const float edit_state[6]);

	RID color_texture() const { return color_; }
	RID hitpos_texture() const { return hitpos_; }

private:
	RID make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h);
	void rebuild_targets(RenderingDevice *rd, const GpuAtlas &atlas, const IslandAtlas *islands,
			RID tile_mask, int w, int h);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_;
	RID sampler_;     // shared NEAREST sampler, created once
	RID edits_ubo_;   // 32-byte uniform buffer, updated every render
	RID material_albedo_, material_surface_, material_sampler_;
	RID color_, hitpos_, uset_, uset_mask_;
	int width_ = 0, height_ = 0;
};

} // namespace godot
