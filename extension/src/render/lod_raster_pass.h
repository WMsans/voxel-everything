#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>

namespace godot {

class LodPool;
class MaterialAtlas;

// Rasterizes the LoD arena with one indexed indirect multi-draw into the scene framebuffer.
// Geometry is pulled from the quad/page/chunk buffers; the CPU supplies one indirect command
// per drawable page (VoxelWorld tracks per-page quad counts because LodPool's page upload
// records them internally but does not expose them).
class LodRasterPass {
public:
	struct PageDraw {
		int page = -1;
		int quad_count = 0;
	};

	~LodRasterPass();
	void initialize(RenderingDevice *rd);
	void teardown();

	// The exact drawable page list for this frame. draw() builds the indirect args from it
	// and uploads them to pool.args_buffer() before opening the draw list.
	void set_draw_pages(const std::vector<PageDraw> &pages);
	void set_cull_enabled(bool enabled) { cull_enabled_ = enabled; }
	// Drops the cached framebuffer. Used by the debug probe before it frees its throwaway
	// colour/depth targets, so the pass never holds a framebuffer pointing at freed textures.
	void release_targets();
	int draw_page_count() const { return static_cast<int>(draw_pages_.size()); }

	bool draw(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
			RID dst_color, RID dst_depth, const Projection &view_proj,
			const float cam_pos[3], int draw_count);

private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth);
	bool ensure_uniform_set(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials);
	bool ensure_index_array(RenderingDevice *rd, LodPool &pool);
	RID active_pipeline() const;

	RenderingDevice *rd_ = nullptr;
	RID shader_;
	RID pipeline_cull_off_;
	RID pipeline_cull_ccw_;
	RID pipeline_cull_cw_;
	RID uset_;
	RID uset_quads_, uset_page_chunk_, uset_chunks_;
	RID uset_albedo_, uset_surface_, uset_sampler_;
	RID index_array_, index_array_buffer_;
	int64_t fb_format_ = 0;
	RID framebuffer_, fb_color_, fb_depth_;
	std::vector<PageDraw> draw_pages_;
	bool cull_enabled_ = true;
	bool front_face_clockwise_ = true; // measured by test_backface_culling_does_not_remove_visible_ground
};

} // namespace godot
