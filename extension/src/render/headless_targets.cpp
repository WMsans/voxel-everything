#include "render/headless_targets.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>

using namespace godot;

bool HeadlessTargets::ensure(RenderingDevice *rd, Vector2i size) {
	if (!rd || size.x <= 0 || size.y <= 0) return false;
	if (rd_ == rd && size_ == size && framebuffer_.is_valid()) return true;
	release();
	rd_ = rd;
	size_ = size;
	auto make = [&](RenderingDevice::DataFormat format, int64_t usage) -> RID {
		Ref<RDTextureFormat> tf;
		tf.instantiate();
		tf->set_format(format);
		tf->set_width(size.x);
		tf->set_height(size.y);
		tf->set_usage_bits(usage);
		Ref<RDTextureView> tv;
		tv.instantiate();
		return rd->texture_create(tf, tv, {});
	};
	color_ = make(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
					RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
					RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	depth_ = make(RenderingDevice::DATA_FORMAT_D32_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
					RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
					RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	if (!color_.is_valid() || !depth_.is_valid()) {
		release();
		return false;
	}
	framebuffer_ = rd->framebuffer_create(Array::make(color_, depth_));
	if (!framebuffer_.is_valid()) {
		release();
		return false;
	}
	return true;
}

bool HeadlessTargets::clear(RenderingDevice *rd) {
	if (!rd || rd != rd_ || !framebuffer_.is_valid()) return false;
	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0));
	// 0.0 is the reverse-Z far plane: inject's GREATER_OR_EQUAL test then accepts every
	// voxel fragment, as it does against the engine's freshly cleared scene depth.
	const int64_t dl = rd->draw_list_begin(framebuffer_,
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH, clears, 0.0f);
	if (dl < 0) return false;
	rd->draw_list_end();
	return true;
}

void HeadlessTargets::release() {
	if (rd_) {
		// Framebuffer first: it depends on both textures.
		if (framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
		if (color_.is_valid()) rd_->free_rid(color_);
		if (depth_.is_valid()) rd_->free_rid(depth_);
	}
	framebuffer_ = RID();
	color_ = RID();
	depth_ = RID();
	rd_ = nullptr;
	size_ = Vector2i(0, 0);
}
