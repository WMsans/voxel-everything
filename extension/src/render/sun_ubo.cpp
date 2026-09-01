#include "render/sun_ubo.h"
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

bool SunUbo::ensure(RenderingDevice *rd) {
	if (!rd) return false;
	if (buffer_.is_valid() && rd_ == rd) return true;
	teardown();
	rd_ = rd;
	PackedByteArray zero;
	zero.resize(32);
	zero.fill(0);
	buffer_ = rd->uniform_buffer_create(32, zero);
	return buffer_.is_valid();
}

void SunUbo::update(RenderingDevice *rd, const ve::SunState &s) {
	if (!rd || !buffer_.is_valid()) return;
	PackedByteArray b;
	b.resize(32);
	float *f = reinterpret_cast<float *>(b.ptrw());
	f[0] = s.dir[0];
	f[1] = s.dir[1];
	f[2] = s.dir[2];
	f[3] = 0.0f;
	f[4] = s.rgb[0];
	f[5] = s.rgb[1];
	f[6] = s.rgb[2];
	f[7] = 0.0f;
	rd->buffer_update(buffer_, 0, 32, b);
}

void SunUbo::teardown() {
	if (rd_ && buffer_.is_valid()) rd_->free_rid(buffer_);
	buffer_ = RID();
	rd_ = nullptr;
}
