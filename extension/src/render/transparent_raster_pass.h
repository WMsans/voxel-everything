#pragma once
#include "render/gpu/gpu.h"
#include "render/lod_raster_pass.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <vector>

namespace godot {

class GBuffer;
class LodPool;

// The far field's transparent shell (docs/superpowers/specs/2026-09-29-transparent-materials-
// design.md §5). Owns the full-resolution far front layer -- front (RGBA32F) and trans
// (RGBA16F), laid out as raymarch.comp.glsl's near targets -- plus its own depth, so the
// nearest shell fragment wins. Draws the LoD arena pages that hold shell quads with one
// indexed indirect draw of its own args; the G-buffer depth is only SAMPLED, never written.
class TransparentRasterPass {
public:
	~TransparentRasterPass();
	void initialize(RenderingDevice *rd);
	void teardown();

	void set_draw_pages(const std::vector<LodRasterPass::PageDraw> &pages) { pages_ = pages; }
	int draw_page_count() const { return static_cast<int>(pages_.size()); }

	// False only on failure. With no shell pages it draws nothing, returns true, and
	// drew() reports false -- the composite then never reads the layer.
	bool draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
			RID beauty_cam_ubo, float fade_start, float fade_end, bool front_face_clockwise);
	// The pages_ term, not just drew_: a frame that skips draw() entirely never clears drew_,
	// and a stale true next to an empty page list would report a far front nobody wrote.
	bool drew() const { return pages_.empty() ? false : drew_; }
	RID front() const { return front_; }
	RID trans() const { return trans_; }

private:
	bool ensure_targets(RenderingDevice *rd, Vector2i size);
	bool ensure_args(RenderingDevice *rd, int pages);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, pipeline_, sampler_;
	bool pipeline_clockwise_ = false;
	gpu::SetCache set_;
	gpu::FramebufferCache framebuffer_;
	RID front_, trans_, depth_, args_;
	Vector2i size_{0, 0};
	int args_capacity_ = 0;
	std::vector<LodRasterPass::PageDraw> pages_;
	bool drew_ = false;
};

} // namespace godot
