#include "render/island_cull_pass.h"
#include "render/island_atlas.h"
#include <cstring>

using namespace godot;

IslandCullPass::~IslandCullPass() {
	teardown();
}

bool IslandCullPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "IslandCullPass", "island_cull.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	return true;
}

void IslandCullPass::teardown() {
	if (rd_) {
		gpu::RdDevice device{rd_};
		group_.release(device);
	}
	program_ = gpu::Program();
	mask_ = RID();
	set_ = gpu::SetCache();
	tiles_x_ = 0;
	tiles_y_ = 0;
	rd_ = nullptr;
}

void IslandCullPass::rebuild_mask(RenderingDevice *rd, int tx, int ty) {
	// Freeing the old mask takes its uniform set with it; the cache rebuilds on the new RID.
	gpu::RdDevice device{rd};
	group_.free(device, mask_);
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(tx) * ty * 4);
	zero.fill(0);
	mask_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(zero.size()), zero));
	tiles_x_ = tx;
	tiles_y_ = ty;
}

bool IslandCullPass::render(RenderingDevice *rd, const IslandAtlas &atlas,
		const ve::CameraParams &cam, int width, int height, int island_count) {
	if (!rd || !is_valid() || width <= 0 || height <= 0 || island_count <= 0) return false;
	const int tx = (width + kIslandTileSize - 1) / kIslandTileSize;
	const int ty = (height + kIslandTileSize - 1) / kIslandTileSize;
	if (tx != tiles_x_ || ty != tiles_y_ || !mask_.is_valid()) rebuild_mask(rd, tx, ty);
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0,
			{gpu::storage(0, atlas.desc_buffer()), gpu::storage(1, mask_)});
	if (!set.is_valid()) return false;

	// The push constant IS ve::CameraParams, with the cull grid in the three trailing ints.
	// Copying rather than re-deriving is the point: the raymarcher gets the same bytes.
	// This private copy also repurposes dims.xy as the ACTUAL raymarch target size: the cull
	// shader's tile NDC must divide by imageSize(out_color), not by the padded 16-multiple
	// grid, or a visible island near a partial edge tile can be assigned to a nonexistent
	// neighboring tile. The raymarcher still receives the original dims.xyz world size.
	ve::CameraParams pc = cam;
	pc.dims[0] = width;
	pc.dims[1] = height;
	pc.dims[3] = island_count;
	pc.region_origin[3] = tx;
	pc.atlas_bricks[3] = ty;
	PackedByteArray b;
	b.resize(sizeof(ve::CameraParams));
	std::memcpy(b.ptrw(), &pc, sizeof(ve::CameraParams));

	// Its own compute list. Godot's RenderingDevice ends a compute list with a full barrier
	// unless told otherwise, so the raymarch list that follows sees the finished mask.
	return gpu::dispatch(rd, program_.pipeline, {{set, 0}}, b, gpu::groups(tx, 8), gpu::groups(ty, 8));
}
