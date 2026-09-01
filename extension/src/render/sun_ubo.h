#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "shade/sun_state.h"

namespace godot {

// One 32-byte uniform buffer holding this frame's sun, bound by the raymarch, deferred and
// contact-shadow passes. Owned by RenderOrchestrator and updated once per frame, so the three
// passes cannot disagree about where the sun is. Mirrors shaders/sun_light.glslh.
class SunUbo {
public:
	bool ensure(RenderingDevice *rd);
	void update(RenderingDevice *rd, const ve::SunState &s);
	RID buffer() const { return buffer_; }
	void teardown();

private:
	RenderingDevice *rd_ = nullptr;
	RID buffer_;
};

} // namespace godot
