#include "render/hiz_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
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

	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/hiz.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("HizPass: shader load failed: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("HizPass: ", compile_err);
		teardown();
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	if (!shader_.is_valid()) {
		teardown();
		return false;
	}
	pipeline_ = rd->compute_pipeline_create(shader_);
	if (!pipeline_.is_valid()) {
		teardown();
		return false;
	}

	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_ = rd->sampler_create(ss);
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
		pyramid_ = rd->texture_create(f, v, {});
		if (!pyramid_.is_valid()) {
			teardown();
			return false;
		}
	}
	{
		Ref<RDTextureFormat> f;
		f.instantiate();
		f->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
		f->set_width(kGrid);
		f->set_height(kGrid);
		f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
		Ref<RDTextureView> v;
		v.instantiate();
		readback_tex_ = rd->texture_create(f, v, {});
		if (!readback_tex_.is_valid()) {
			teardown();
			return false;
		}
	}

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
	// Mip 0's source is the frame's scene depth and is cached lazily in build().
	for (int m = 1; m < kMipCount; m++) {
		if (!ensure_uniform_set(rd, slices_[m - 1], m)) {
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
	// Free order: uniform sets reference the shader, and freeing a texture cascades to its
	// shared slices and referencing sets, so sets first, then pipeline/shader, then the
	// texture/sampler resources.
	for (RID &r : usets_) {
		if (r.is_valid()) rd_->free_rid(r);
		r = RID();
	}
	uset0_src_ = RID();
	if (pipeline_.is_valid()) rd_->free_rid(pipeline_);
	pipeline_ = RID();
	if (shader_.is_valid()) rd_->free_rid(shader_);
	shader_ = RID();
	if (sampler_.is_valid()) rd_->free_rid(sampler_);
	sampler_ = RID();
	if (readback_tex_.is_valid()) rd_->free_rid(readback_tex_);
	readback_tex_ = RID();
	// Shared slice RIDs are views of pyramid_; freeing pyramid_ frees them automatically.
	if (pyramid_.is_valid()) rd_->free_rid(pyramid_);
	pyramid_ = RID();
	for (RID &r : slices_) r = RID();
	readback_ = Ref<AsyncTextureRead>();
	occlusion_ = HizOcclusion();
	rd_ = nullptr;
}

bool HizPass::ensure_uniform_set(RenderingDevice *rd, RID src, int dst_mip) {
	if (dst_mip == 0) {
		if (usets_[0].is_valid() && src == uset0_src_) return true;
		if (usets_[0].is_valid()) rd->free_rid(usets_[0]);
		usets_[0] = RID();
	} else if (usets_[dst_mip].is_valid()) {
		return true;
	}

	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0);
	u0->add_id(sampler_);
	u0->add_id(src);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u1->set_binding(1);
	u1->add_id(slices_[dst_mip]);
	usets_[dst_mip] = rd->uniform_set_create(Array::make(u0, u1), shader_, 0);
	if (!usets_[dst_mip].is_valid()) return false;
	if (dst_mip == 0) uset0_src_ = src;
	return true;
}

bool HizPass::build(RenderingDevice *rd, RID scene_depth, Vector2i scene_size) {
	if (!rd_ || !pipeline_.is_valid() || !readback_.is_valid()) return false;
	if (scene_size.x <= 0 || scene_size.y <= 0) return false;

	if (readback_->take_fresh()) occlusion_.update(readback_->data());
	if (!ensure_uniform_set(rd, scene_depth, 0)) return false;

	const int64_t list = rd->compute_list_begin();
	for (int m = 0; m < kMipCount; m++) {
		const int dw = size_at(m);
		const int dh = size_at(m);
		const int sw = m == 0 ? scene_size.x : size_at(m - 1);
		const int sh = m == 0 ? scene_size.y : size_at(m - 1);

		rd->compute_list_bind_compute_pipeline(list, pipeline_);
		rd->compute_list_bind_uniform_set(list, usets_[m], 0);
		PackedByteArray pc;
		pc.resize(32);
		int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
		p[0] = dw;
		p[1] = dh;
		p[2] = sw;
		p[3] = sh;
		p[4] = m == 0 ? 1 : 0;
		p[5] = p[6] = p[7] = 0;
		rd->compute_list_set_push_constant(list, pc, pc.size());
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
	if (usets_[0].is_valid()) rd_->free_rid(usets_[0]);
	usets_[0] = RID();
	uset0_src_ = RID();
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
