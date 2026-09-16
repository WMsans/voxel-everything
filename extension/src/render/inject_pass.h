#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/gpu/gpu.h"

namespace godot {

class InjectPass {
public:
	~InjectPass();
	void initialize(RenderingDevice *rd);
	void teardown();
	void release_targets();
	bool draw(RenderingDevice *rd, RID dst_color, RID dst_depth, RID lit, RID gb_depth);

private:
	bool ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	RID shader_, pipeline_, sampler_linear_, sampler_nearest_;
	gpu::FramebufferCache framebuffer_;
	gpu::SetCache set_;
};

} // namespace godot
