#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "world/world_data.h"

namespace godot {

// Owns the GPU mirror of a ve::WorldData: brick atlases, indirection, palette.
class GpuWorld {
public:
	static constexpr int kAtlasBricksX = 32;
	static constexpr int kAtlasBricksY = 16;
	static constexpr int kAtlasBricksZ = 32;
	static constexpr int kAtlasBrickCount = kAtlasBricksX * kAtlasBricksY * kAtlasBricksZ; // 16384

	bool initialize(RenderingDevice *rd, const ve::WorldData &world);
	void teardown();

	RID sdf_atlas() const { return sdf_atlas_; }
	RID mat_atlas() const { return mat_atlas_; }
	RID indirection_tex() const { return indirection_; }
	RID palette_buffer() const { return palette_; }

private:
	RenderingDevice *rd_ = nullptr;
	RID sdf_atlas_, mat_atlas_, indirection_, palette_;
};

} // namespace godot
