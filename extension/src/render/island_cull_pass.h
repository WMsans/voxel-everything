#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"

namespace godot {

class IslandAtlas;

// Spec §3's 16 x 16 px tiles.
inline constexpr int kIslandTileSize = 16;

// Writes one uint per screen tile: bit i means "island i may be visible in this tile". The
// raymarcher then marches only those bits, which is what keeps the per-pixel cost at spec
// §3's "0-3 islands" instead of all 32.
class IslandCullPass {
public:
	~IslandCullPass();

	bool initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }

	// Sizes the mask to the raymarch target, records its own compute list and dispatches.
	// Returns false when nothing was recorded (in which case the caller passes RID() to the
	// raymarcher and every live island is marched -- correct, just slower).
	bool render(RenderingDevice *rd, const IslandAtlas &atlas, const ve::CameraParams &cam,
			int width, int height, int island_count);

	RID mask_buffer() const { return mask_; }
	int tiles_x() const { return tiles_x_; }
	int tiles_y() const { return tiles_y_; }

private:
	void rebuild(RenderingDevice *rd, const IslandAtlas &atlas, int tx, int ty);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, uset_, mask_;
	int tiles_x_ = 0, tiles_y_ = 0;
};

} // namespace godot
