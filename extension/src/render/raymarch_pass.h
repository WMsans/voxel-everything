#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"

namespace godot {

class GpuWorld;

class RaymarchPass {
public:
	~RaymarchPass(); // calls teardown(): frees RIDs on rd_ (device must be alive)
	void initialize(RenderingDevice *rd);
	void teardown();
	bool render(RenderingDevice *rd, const GpuWorld &world, const ve::CameraParams &cam,
			int width, int height);

	RID color_texture() const { return color_; }
	RID hitpos_texture() const { return hitpos_; }

private:
	RID make_target(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h);
	void rebuild_targets(RenderingDevice *rd, const GpuWorld &world, int w, int h);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_;
	RID sampler_; // shared NEAREST sampler, created once
	RID color_, hitpos_, uset_;
	int width_ = 0, height_ = 0;
};

} // namespace godot
