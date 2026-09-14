#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot {

// The scene colour and depth a headless frame injects into and post-processes -- the local
// device's stand-in for Godot's RenderSceneBuffersRD colour/depth. Formats match what the
// engine hands the compositors (RGBA16F, D32F). Cleared through a render pass every frame:
// texture_clear is a colour clear and Metal refuses it on a depth format
// (LodRasterPass::clear_targets).
//
// The destructor frees NOTHING: by the time a VoxelWorld is destroyed its local device is
// already gone. release() is the only free path; VoxelWorld calls it before dropping devices.
class HeadlessTargets {
public:
	bool ensure(RenderingDevice *rd, Vector2i size);
	bool clear(RenderingDevice *rd);
	void release();
	RID color() const { return color_; }
	RID depth() const { return depth_; }
	Vector2i size() const { return size_; }

private:
	RenderingDevice *rd_ = nullptr;
	Vector2i size_{0, 0};
	RID color_, depth_, framebuffer_;
};

} // namespace godot
