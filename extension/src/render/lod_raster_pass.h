#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>

namespace godot {

class LodPool;
class MaterialAtlas;
class GBuffer;

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

	// The exact drawable page list for this frame. The compositor uploads the indirect args
	// to pool.args_buffer() (LodPool::upload_draw_args) before the cull pass, then draw()
	// only opens the draw list and issues the indirect draw.
	void set_draw_pages(const std::vector<PageDraw> &pages);
	const std::vector<PageDraw> &draw_pages() const { return draw_pages_; }
	void set_cull_enabled(bool enabled) { cull_enabled_ = enabled; }
	// Drops the cached framebuffer. Used by the debug probe before it frees its throwaway
	// colour/depth targets, so the pass never holds a framebuffer pointing at freed textures.
	void release_targets();
	int draw_page_count() const { return static_cast<int>(draw_pages_.size()); }
	// CPU command-record time only: std::chrono around draw()'s command recording, not GPU
	// execution time. VoxelWorld::debug_perf_stats() reports this as lod_ms.
	float last_ms() const { return last_ms_; }
	// Task 8's depth-only shadow pipeline inherits this rather than re-deriving it: the
	// winding was MEASURED (M5 errata 2), and a second derivation is a second chance to get
	// it wrong.
	bool front_face_clockwise() const { return front_face_clockwise_; }
	RID index_array() const { return index_array_; }

	bool draw(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
			GBuffer &gb, const Projection &view_proj,
			const float cam_pos[3], int draw_count,
			float fade_start, float fade_end, RID marker = RID());

private:
	bool ensure_pipeline(RenderingDevice *rd, GBuffer &gb, RID marker);
	bool ensure_uniform_set(RenderingDevice *rd, LodPool &pool, MaterialAtlas &materials,
			RID shader);
	bool ensure_index_array(RenderingDevice *rd, LodPool &pool);
	RID active_pipeline() const;

	RenderingDevice *rd_ = nullptr;
	RID shader_, shader_marker_;
	RID pipeline_cull_off_;
	RID pipeline_cull_ccw_;
	RID pipeline_cull_cw_;
	RID uset_, uset_shader_;
	RID uset_quads_, uset_page_chunk_, uset_chunks_;
	RID uset_albedo_, uset_surface_, uset_sampler_;
	RID index_array_, index_array_buffer_;
	int64_t fb_format_ = 0;
	bool pipeline_marker_ = false;
	RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_, fb_marker_;
	std::vector<PageDraw> draw_pages_;
	float last_ms_ = 0.0f;
	bool cull_enabled_ = true;
	// The scene projection flips Y, which mirrors clip space and reverses screen-space
	// winding, so the pre-wound quads present COUNTER-clockwise front faces. Measured by
	// test_backface_culling_does_not_remove_visible_ground.
	bool front_face_clockwise_ = false;
};

} // namespace godot
