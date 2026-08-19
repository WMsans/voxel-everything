#include "render/beauty_camera.h"
#include <cstring>

using namespace godot;

namespace {
static_assert(sizeof(float) * 40 == 160, "beauty camera block");
}

bool CameraUbo::ensure(RenderingDevice *rd) {
	if (!rd) return false;
	if (rd_ == rd && buffer_.is_valid()) return true;
	teardown();
	PackedByteArray zero;
	zero.resize(160);
	zero.fill(0);
	buffer_ = rd->uniform_buffer_create(160, zero);
	if (!buffer_.is_valid()) return false;
	rd_ = rd;
	return true;
}

void CameraUbo::update(RenderingDevice *rd, const Projection &view_proj, const float cam_pos[3],
		Vector2i size, float z_near, float z_far) {
	(void)z_near;
	(void)z_far;
	if (!rd || !buffer_.is_valid() || size.x <= 0 || size.y <= 0) return;
	const Projection inv = view_proj.inverse();
	PackedByteArray bytes;
	bytes.resize(160);
	float *f = reinterpret_cast<float *>(bytes.ptrw());
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			f[c * 4 + r] = view_proj.columns[c][r];
			f[16 + c * 4 + r] = inv.columns[c][r];
		}
	f[32] = cam_pos[0];
	f[33] = cam_pos[1];
	f[34] = cam_pos[2];
	f[35] = 0.0f;
	f[36] = static_cast<float>(size.x);
	f[37] = static_cast<float>(size.y);
	f[38] = 1.0f / static_cast<float>(size.x);
	f[39] = 1.0f / static_cast<float>(size.y);
	// Device-level operation: callers must perform this before opening a list.
	rd->buffer_update(buffer_, 0, 160, bytes);
}

void CameraUbo::teardown() {
	if (rd_ && buffer_.is_valid()) rd_->free_rid(buffer_);
	buffer_ = RID();
	rd_ = nullptr;
}
