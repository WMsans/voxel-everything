#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu/gpu.h"

namespace godot {

class ConiferScatterPass;
class GBuffer;

// One non-indexed indirect draw of six vertices per imposter tree, pulled from the cull's
// imposter list. The fragment ray-casts the tiered crown and writes real depth, so imposters
// sort against terrain and each other with the far field's reverse-Z compare (spec §6).
class ConiferImpostorPass {
public:
	~ConiferImpostorPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	void release_targets();
	// False on failure; true with no draw when nothing was listed.
	bool draw(RenderingDevice *rd, ConiferScatterPass &scatter, GBuffer &gb,
			const Projection &view_proj, const float cam_pos[3]);
	int last_vertex_count() const { return last_vertex_count_; }

private:
	bool ensure_pipeline(RenderingDevice *rd, GBuffer &gb);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, pipeline_;
	gpu::FramebufferCache framebuffer_;
	gpu::SetCache set_;
	int last_vertex_count_ = 0;
};

} // namespace godot
