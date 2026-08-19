#include "render/gbuffer.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <algorithm>

using namespace godot;

const char *GBuffer::kContext = "voxel_gbuf";

namespace {

struct TargetSpec {
	const char *name;
	RenderingDevice::DataFormat format;
	uint32_t usage;
	bool half;
};

// One table, read by both the managed and the owned path, so the two can never allocate
// different formats for the same name.
const TargetSpec kTargets[5] = {
	{"albedo", RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM,
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT, false},
	{"surface", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT, false},
	{"depth", RenderingDevice::DATA_FORMAT_D32_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT, false},
	{"lit", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT, false},
	{"history", RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT, true},
};

Vector2i half_of(Vector2i s) {
	return Vector2i(std::max(1, s.x / 2), std::max(1, s.y / 2));
}

} // namespace

GBuffer::~GBuffer() {
	teardown();
}

Vector2i GBuffer::half_size() const {
	return half_of(size_);
}

bool GBuffer::is_valid() const {
	return albedo_.is_valid() && surface_.is_valid() && depth_.is_valid() &&
			lit_.is_valid() && history_.is_valid();
}

bool GBuffer::ensure(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size) {
	if (!rd || size.x <= 0 || size.y <= 0) return false;
	rd_ = rd;
	return rsb ? ensure_managed(rsb, size) : ensure_owned(rd, size);
}

bool GBuffer::ensure_managed(RenderSceneBuffersRD *rsb, Vector2i size) {
	// If this object previously owned textures (a probe ran first), let go of them.
	if (owned_) free_owned();
	owned_ = false;
	RID *slots[5] = {&albedo_, &surface_, &depth_, &lit_, &history_};
	for (int i = 0; i < 5; i++) {
		const TargetSpec &t = kTargets[i];
		const Vector2i ts = t.half ? half_of(size) : size;
		// Re-query every frame rather than caching: the engine drops the whole context on a
		// viewport reconfigure, and has_texture() going false is the ONLY signal it does.
		if (!rsb->has_texture(kContext, t.name)) {
			*slots[i] = rsb->create_texture(kContext, t.name, t.format, t.usage,
					RenderingDevice::TEXTURE_SAMPLES_1, ts, 1, 1, true, false);
			reallocations_++;
		} else {
			*slots[i] = rsb->get_texture(kContext, t.name);
		}
		if (!slots[i]->is_valid()) return false;
	}
	size_ = size;
	return true;
}

bool GBuffer::ensure_owned(RenderingDevice *rd, Vector2i size) {
	if (owned_ && size == size_ && is_valid()) return true;
	free_owned();
	owned_ = true;
	RID *slots[5] = {&albedo_, &surface_, &depth_, &lit_, &history_};
	for (int i = 0; i < 5; i++) {
		const TargetSpec &t = kTargets[i];
		const Vector2i ts = t.half ? half_of(size) : size;
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(t.format);
		f->set_width(ts.x);
		f->set_height(ts.y);
		f->set_usage_bits(t.usage);
		Ref<RDTextureView> v;
		v.instantiate();
		*slots[i] = rd->texture_create(f, v, TypedArray<PackedByteArray>());
		if (!slots[i]->is_valid()) {
			free_owned();
			return false;
		}
	}
	reallocations_++;
	size_ = size;
	return true;
}

void GBuffer::free_owned() {
	if (!owned_ || !rd_) {
		albedo_ = surface_ = depth_ = lit_ = history_ = RID();
		owned_ = false;
		return;
	}
	for (RID *r : {&albedo_, &surface_, &depth_, &lit_, &history_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	owned_ = false;
}

void GBuffer::teardown() {
	free_owned();
	albedo_ = surface_ = depth_ = lit_ = history_ = RID();
	size_ = Vector2i(0, 0);
	rd_ = nullptr;
}
