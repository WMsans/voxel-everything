#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot {

class CameraUbo {
public:
	bool ensure(RenderingDevice *rd);
	void update(RenderingDevice *rd, const Projection &view_proj, const float cam_pos[3],
			Vector2i size, float z_near, float z_far);
	RID buffer() const { return buffer_; }
	void teardown();

private:
	RenderingDevice *rd_ = nullptr;
	RID buffer_;
};

} // namespace godot
