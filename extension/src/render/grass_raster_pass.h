#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

class GrassScatterPass;
class GBuffer;

// Draws one non-indexed indirect triangle list into the scene G-buffer: three vertices per
// blade, straight and unanimated. Geometry is PULLED from the scatter pass's instance
// buffer (gl_VertexIndex / 3 is the blade, % 3 the corner), exactly as LodRasterPass pulls
// quads -- Godot exposes neither gl_DrawID nor a non-zero firstInstance. Blades write the
// same albedo/surface/depth channels the far field writes, so the beauty stack below
// shades them unchanged.
class GrassRasterPass {
public:
	~GrassRasterPass();
	void initialize(RenderingDevice *rd);
	void teardown();

	// Drops the cached framebuffer. Used by the debug hook before it frees its throwaway
	// colour/depth targets, so the pass never holds a framebuffer pointing at freed
	// textures. Matches LodRasterPass::release_targets.
	void release_targets();

	// One non-indexed indirect draw; the vertex count is whatever the scatter wrote.
	// False on any failure; the caller cancels the timing marker and skips grass. Never
	// aborts the frame -- grass is decorative. True with no draw when nothing was placed
	// (invalid scatter buffers): not a failure.
	bool draw(RenderingDevice *rd, GrassScatterPass &scatter, GBuffer &gb,
			const Projection &view_proj, const float cam_pos[3]);

	// Blades the last draw() issued vertices for (three per blade). Zero when the last
	// draw did not run or placed nothing; read by debug_grass_stats().
	int last_vertex_count() const { return last_vertex_count_; }

private:
	bool ensure_pipeline(RenderingDevice *rd, GBuffer &gb);
	bool ensure_uniform_set(RenderingDevice *rd, GrassScatterPass &scatter);

	RenderingDevice *rd_ = nullptr;
	RID shader_;
	RID pipeline_;
	RID uset_, uset_shader_;
	RID uset_instances_, uset_params_;
	int64_t fb_format_ = 0;
	RID framebuffer_, fb_albedo_, fb_surface_, fb_depth_;
	int last_vertex_count_ = 0;
};

} // namespace godot
