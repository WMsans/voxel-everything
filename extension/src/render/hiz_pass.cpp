#include "render/hiz_pass.h"
#include "gpu_layout/blocks.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/array.hpp>
#include <cmath>
#include <cstring>

using namespace godot;

HizPass::~HizPass() {
	teardown();
}

int HizPass::size_at(int level) const {
	if (level <= 0) return kSize;
	if (level >= kMipCount) return 1;
	return kSize >> level;
}

void HizPass::HizOcclusion::update(const PackedByteArray &data) {
	const int need = HizPass::kGrid * HizPass::kGrid * 4;
	if (data.size() < need) return;
	std::memcpy(grid_, data.ptr(), static_cast<size_t>(need));
	have_data_ = true;
}

bool HizPass::HizOcclusion::occluded(const float ss_min[3], const float ss_max[3]) const {
	if (!have_data_) return false; // no readback yet: the safe answer is always "visible"
	const int lo_x = std::max(0, int(std::floor(ss_min[0] * HizPass::kGrid)));
	const int hi_x = std::min(HizPass::kGrid - 1, int(std::ceil(ss_max[0] * HizPass::kGrid)) - 1);
	const int lo_y = std::max(0, int(std::floor(ss_min[1] * HizPass::kGrid)));
	const int hi_y = std::min(HizPass::kGrid - 1, int(std::ceil(ss_max[1] * HizPass::kGrid)) - 1);
	if (lo_x > hi_x || lo_y > hi_y) return false;
	float occluder = 1.0f;
	for (int y = lo_y; y <= hi_y; y++)
		for (int x = lo_x; x <= hi_x; x++)
			occluder = std::min(occluder, grid_[y * HizPass::kGrid + x]);
	// Reverse-Z: the node's NEAREST point is its largest depth. If even that is behind the
	// farthest occluder over its footprint, everything in the node is behind everything
	// drawn there.
	return ss_max[2] < occluder;
}

bool HizPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	program_ = gpu::compile_compute(rd, group_, "HizPass", "hiz.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!sampler_.is_valid()) {
		teardown();
		return false;
	}

	{
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
		f->set_width(kSize);
		f->set_height(kSize);
		f->set_mipmaps(kMipCount);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		pyramid_ = group_.add(gpu::Kind::Texture, rd->texture_create(f, v, {}));
		if (!pyramid_.is_valid()) {
			teardown();
			return false;
		}
	}
	readback_tex_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32_SFLOAT,
			Vector2i(kGrid, kGrid),
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	if (!readback_tex_.is_valid()) {
		teardown();
		return false;
	}

	// Views of pyramid_: freeing pyramid_ frees them, so they are not registered.
	for (int m = 0; m < kMipCount; m++) {
		Ref<RDTextureView> v;
		v.instantiate();
		slices_[m] = rd->texture_create_shared_from_slice(v, pyramid_, 0, m, 1,
				RenderingDevice::TEXTURE_SLICE_2D);
		if (!slices_[m].is_valid()) {
			teardown();
			return false;
		}
	}

	readback_.instantiate();
	if (readback_.is_null()) {
		teardown();
		return false;
	}

	// Mips 1..8 have fixed source/destination slices, so their uniform sets are built once.
	// Mip 0's source is the frame's scene depth and is cached in build().
	for (int m = 1; m < kMipCount; m++) {
		usets_[m] = gpu::uniform_set(rd, group_, program_.shader, 0,
				{gpu::sampled(0, sampler_, slices_[m - 1]), gpu::image(1, slices_[m])});
		if (!usets_[m].is_valid()) {
			teardown();
			return false;
		}
	}
	return true;
}

void HizPass::teardown() {
	if (!rd_) return;
	// RenderingDevice retains the Callable for an async readback but not this RefCounted target.
	// Drain before freeing the source texture or releasing readback_, otherwise the deferred
	// callback can validate a freed ObjectDB entry during allocator cleanup.
	readback_was_pending_at_teardown_ = readback_.is_valid() && readback_->pending();
	readback_was_drained_at_teardown_ = !readback_was_pending_at_teardown_ || readback_->drain(rd_);
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_ = pyramid_ = readback_tex_ = RID();
	for (RID &r : slices_) r = RID();
	for (RID &r : usets_) r = RID();
	level0_ = gpu::SetCache();
	readback_ = Ref<AsyncTextureRead>();
	occlusion_ = HizOcclusion();
	rd_ = nullptr;
}

bool HizPass::build(RenderingDevice *rd, RID scene_depth, Vector2i scene_size) {
	if (!rd_ || !program_.valid() || !readback_.is_valid()) return false;
	if (scene_size.x <= 0 || scene_size.y <= 0) return false;

	if (readback_->take_fresh()) occlusion_.update(readback_->data());
	gpu::RdDevice device{rd};
	usets_[0] = level0_.get(device, group_, program_.shader, 0,
			{gpu::sampled(0, sampler_, scene_depth), gpu::image(1, slices_[0])});
	if (!usets_[0].is_valid()) return false;

	const int64_t list = rd->compute_list_begin();
	// A failed list can occur during viewport/device teardown. Return before any binding or
	// dispatch so the caller can take the conservative visible/no-HiZ path.
	if (list < 0) return false;
	for (int m = 0; m < kMipCount; m++) {
		const int dw = size_at(m);
		const int dh = size_at(m);
		const int sw = m == 0 ? scene_size.x : size_at(m - 1);
		const int sh = m == 0 ? scene_size.y : size_at(m - 1);

		rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
		rd->compute_list_bind_uniform_set(list, usets_[m], 0);
		const ve::HizPush push{{dw, dh, sw, sh}, {m == 0 ? 1 : 0, 0, 0, 0}};
		rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
		rd->compute_list_dispatch(list, (static_cast<uint32_t>(dw) + 7) / 8,
				(static_cast<uint32_t>(dh) + 7) / 8, 1);
		if (m + 1 < kMipCount) rd->compute_list_add_barrier(list);
	}
	rd->compute_list_end();

	// The CPU readback is only 4 KB: copy the 32^2 mip-3 slice into a single-mip texture so
	// texture_get_data_async downloads that texel block instead of the whole 349 KB layer.
	rd->texture_copy(pyramid_, readback_tex_, Vector3(0, 0, 0), Vector3(0, 0, 0),
			Vector3(kGrid, kGrid, 1), kReadbackLevel, 0, 0, 0);
	readback_->request(rd, readback_tex_);
	return true;
}

void HizPass::release_level0_set() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	level0_.drop(device, group_);
	usets_[0] = RID();
}

bool HizPass::update_occlusion(const PackedByteArray &data) {
	if (data.size() < kGrid * kGrid * 4) return false;
	occlusion_.update(data);
	return true;
}

float HizPass::probe_mip_texel(RenderingDevice *rd, int level, int x, int y) const {
	if (!rd || !pyramid_.is_valid() || level < 0 || level >= kMipCount) return 0.0f;
	const int w = size_at(level);
	const int h = size_at(level);
	if (x < 0 || x >= w || y < 0 || y >= h) return 0.0f;
	const PackedByteArray data = rd->texture_get_data(pyramid_, 0);
	int64_t off = 0;
	for (int m = 0; m < level; m++) {
		const int s = size_at(m);
		off += static_cast<int64_t>(s) * s * 4;
	}
	off += (static_cast<int64_t>(y) * w + x) * 4;
	if (data.size() < off + 4) return 0.0f;
	float v = 0.0f;
	std::memcpy(&v, data.ptr() + off, 4);
	return v;
}
