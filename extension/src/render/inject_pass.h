#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>

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
	RID shader_, pipeline_, sampler_linear_, sampler_nearest_;
	RID framebuffer_, fb_color_, fb_depth_;
	RID uset_, uset_lit_, uset_depth_;
	int64_t fb_format_ = 0;
};

} // namespace godot
