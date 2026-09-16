#include "render/beauty_camera.h"
#include "render/gpu/gpu.h"
#include "gpu_layout/blocks.h"
#include <cstring>

using namespace godot;

bool CameraUbo::ensure(RenderingDevice *rd) {
	if (!rd) return false;
	if (rd_ == rd && buffer_.is_valid()) return true;
	teardown();
	PackedByteArray zero;
	zero.resize(sizeof(ve::BeautyCamBlock));
	zero.fill(0);
	buffer_ = rd->uniform_buffer_create(sizeof(ve::BeautyCamBlock), zero);
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
	ve::BeautyCamBlock block{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) {
			block.view_proj[c * 4 + r] = view_proj.columns[c][r];
			block.inv_view_proj[c * 4 + r] = inv.columns[c][r];
		}
	block.cam[0] = cam_pos[0];
	block.cam[1] = cam_pos[1];
	block.cam[2] = cam_pos[2];
	block.screen[0] = static_cast<float>(size.x);
	block.screen[1] = static_cast<float>(size.y);
	block.screen[2] = 1.0f / static_cast<float>(size.x);
	block.screen[3] = 1.0f / static_cast<float>(size.y);
	// Device-level operation: callers must perform this before opening a list.
	rd->buffer_update(buffer_, 0, sizeof(block), gpu::push_bytes(block));
}

void CameraUbo::teardown() {
	if (rd_ && buffer_.is_valid()) rd_->free_rid(buffer_);
	buffer_ = RID();
	rd_ = nullptr;
}
