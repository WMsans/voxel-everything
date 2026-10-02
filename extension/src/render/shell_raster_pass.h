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

// The transparent shell's two raster passes (docs/superpowers/specs/2026-10-01-transparent-
// voxels-design.md §6). Owns three full-resolution targets:
//   thickness  RG32F    additive: R = back distances - front distances, G = fronts - backs
//   front      RGBA32F  xy oct normal, z distance, w material id (0 = none)
//   depth      D32      the front pass's own depth; sampled by the G-buffer resolve
// The thickness pass depth-tests against the G-buffer's depth without writing it; the front
// pass only SAMPLES the G-buffer depth. Neither writes the G-buffer.
class ShellRasterPass {
public:
	~ShellRasterPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	// Drops the framebuffers that reference the G-buffer depth (headless reallocation).
	void release_targets();

	// Every frame the pass is offered its pages, drawn or not: drew() means "these targets are
	// this frame's", so a frame that never reaches draw() must not keep the last frame's true.
	void set_draw_pages(const std::vector<LodRasterPass::PageDraw> &pages) {
		pages_ = pages;
		drew_ = false;
	}
	int draw_page_count() const { return static_cast<int>(pages_.size()); }

	// False only on failure. With no pages and the camera outside it draws nothing, returns
	// true, and drew() reports false -- the composite then never reads the targets.
	// camera_inside: the camera sits in a transparent solid, so the thickness target is
	// cleared to a virtual front at distance 0.
	bool draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
			RID beauty_cam_ubo, RID island_desc, float fade_start, float fade_end,
			bool front_face_clockwise, bool camera_inside);
	bool drew() const { return drew_; }
	// The targets are deliberately left STALE on a frame that drew nothing, so these report
	// RID() rather than last frame's texture. That is the enforcement, not a convention every
	// reader has to remember: every consumer (the composite, inject, the debug hooks) is
	// already gated on validity, so a stale frame is refused at the one place they all reach.
	RID thickness() const { return drew_ ? thick_ : RID(); }
	RID front() const { return drew_ ? front_ : RID(); }
	RID front_depth() const { return drew_ ? depth_ : RID(); }

private:
	bool ensure_targets(RenderingDevice *rd, Vector2i size);
	bool ensure_args(RenderingDevice *rd, int pages);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID thick_shader_, front_shader_, thick_pipeline_, front_pipeline_, sampler_;
	bool pipeline_clockwise_ = false;
	gpu::SetCache thick_set_, front_set_;
	gpu::FramebufferCache thick_fb_, front_fb_;
	RID thick_, front_, depth_, args_;
	Vector2i size_{0, 0};
	int args_capacity_ = 0;
	std::vector<LodRasterPass::PageDraw> pages_;
	bool drew_ = false;
};

} // namespace godot
