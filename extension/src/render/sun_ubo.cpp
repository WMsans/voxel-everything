#include "render/sun_ubo.h"
#include "render/gpu/gpu.h"
#include "gpu_layout/blocks.h"

using namespace godot;

bool SunUbo::ensure(RenderingDevice *rd) {
	if (!rd) return false;
	if (buffer_.is_valid() && rd_ == rd) return true;
	teardown();
	rd_ = rd;
	PackedByteArray zero;
	zero.resize(sizeof(ve::SunLightBlock));
	zero.fill(0);
	buffer_ = rd->uniform_buffer_create(sizeof(ve::SunLightBlock), zero);
	return buffer_.is_valid();
}

void SunUbo::update(RenderingDevice *rd, const ve::SunState &s) {
	if (!rd || !buffer_.is_valid()) return;
	const ve::SunLightBlock block{{s.dir[0], s.dir[1], s.dir[2], 0.0f},
			{s.rgb[0], s.rgb[1], s.rgb[2], 0.0f}};
	rd->buffer_update(buffer_, 0, sizeof(block), gpu::push_bytes(block));
}

void SunUbo::teardown() {
	if (rd_ && buffer_.is_valid()) rd_->free_rid(buffer_);
	buffer_ = RID();
	rd_ = nullptr;
}
