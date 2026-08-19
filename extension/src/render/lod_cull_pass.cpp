#include "render/lod_cull_pass.h"
#include "lod/lod_contour.h"
#include "render/hiz_pass.h"
#include "render/lod_pool.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <chrono>
#include <cstring>

using namespace godot;

LodCullPass::~LodCullPass() {
	teardown();
}

bool LodCullPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/lod_cull.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("LodCullPass: shader load failed: ", err.c_str());
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
		UtilityFunctions::printerr("LodCullPass: ", compile_err);
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
		UtilityFunctions::printerr("LodCullPass: pipeline creation failed");
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

	PackedByteArray zero;
	zero.resize(4);
	zero.fill(0);
	stats_ = rd->storage_buffer_create(4, zero);
	if (!stats_.is_valid()) {
		teardown();
		return false;
	}

	stats_readback_.instantiate();
	if (stats_readback_.is_null()) {
		teardown();
		return false;
	}
	args_readback_.instantiate();
	if (args_readback_.is_null()) {
		teardown();
		return false;
	}
	return true;
}

void LodCullPass::teardown() {
	if (!rd_) return;
	// Complete pending staging-buffer callbacks while their source buffer and RefCounted
	// targets are still alive; RenderingDevice has no cancellation operation.
	if (stats_readback_.is_valid()) stats_readback_->drain(rd_);
	if (args_readback_.is_valid()) args_readback_->drain(rd_);
	// Uniform set first: it references the shader, stats, and pool buffers.
	if (uset_.is_valid()) rd_->free_rid(uset_);
	uset_ = RID();
	if (pipeline_.is_valid()) rd_->free_rid(pipeline_);
	pipeline_ = RID();
	if (shader_.is_valid()) rd_->free_rid(shader_);
	shader_ = RID();
	if (sampler_.is_valid()) rd_->free_rid(sampler_);
	sampler_ = RID();
	if (stats_.is_valid()) rd_->free_rid(stats_);
	stats_ = RID();
	uset_args_ = RID();
	uset_page_chunk_ = RID();
	uset_chunks_ = RID();
	uset_hiz_ = RID();
	uset_stats_ = RID();
	stats_readback_ = Ref<AsyncBufferRead>();
	args_readback_ = Ref<AsyncBufferRead>();
	first_pass_pages_.clear();
	first_pass_pages_at_request_.clear();
	last_visible_pages_.clear();
	last_drawn_ = 0;
	last_total_ = 0;
	last_total_at_request_ = 0;
	last_first_pass_count_at_request_ = 0;
	last_remaining_count_at_request_ = 0;
	rd_ = nullptr;
}

bool LodCullPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, HizPass *hiz) {
	if (!hiz || !hiz->pyramid().is_valid()) return false;
	const RID args = pool.args_buffer();
	const RID page_chunk = pool.page_chunk_buffer();
	const RID chunks = pool.chunk_buffer();
	const RID hiz_tex = hiz->pyramid();
	if (uset_.is_valid() && args == uset_args_ && page_chunk == uset_page_chunk_ &&
			chunks == uset_chunks_ && hiz_tex == uset_hiz_ && stats_ == uset_stats_) {
		return true;
	}
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u0->set_binding(0);
	u0->add_id(args);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u1->set_binding(1);
	u1->add_id(page_chunk);
	Ref<RDUniform> u2;
	u2.instantiate();
	u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u2->set_binding(2);
	u2->add_id(chunks);
	Ref<RDUniform> u3;
	u3.instantiate();
	u3->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u3->set_binding(3);
	u3->add_id(sampler_);
	u3->add_id(hiz_tex);
	Ref<RDUniform> u4;
	u4.instantiate();
	u4->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u4->set_binding(4);
	u4->add_id(stats_);
	uset_ = rd->uniform_set_create(Array::make(u0, u1, u2, u3, u4), shader_, 0);
	if (!uset_.is_valid()) return false;
	uset_args_ = args;
	uset_page_chunk_ = page_chunk;
	uset_chunks_ = chunks;
	uset_hiz_ = hiz_tex;
	uset_stats_ = stats_;
	return true;
}

void LodCullPass::consume_args_readback() {
	const PackedByteArray &data = args_readback_->data();
	const int remaining_count = last_remaining_count_at_request_;
	if (remaining_count <= 0 || data.size() < static_cast<int64_t>(remaining_count) * 20) {
		return; // fail-soft: keep the previous visible set
	}

	std::vector<int> visible = first_pass_pages_at_request_;
	const uint32_t *a = reinterpret_cast<const uint32_t *>(data.ptr());
	for (int i = 0; i < remaining_count; i++) {
		if (a[static_cast<size_t>(i) * 5u + 1u] != 0u) {
			const uint32_t page = a[static_cast<size_t>(i) * 5u + 3u] /
					static_cast<uint32_t>(ve::kLodVertsPerPage);
			visible.push_back(static_cast<int>(page));
		}
	}
	std::sort(visible.begin(), visible.end());
	visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
	last_visible_pages_.swap(visible);
}

void LodCullPass::set_last_visible_pages(const std::vector<int> &pages) {
	last_visible_pages_ = pages;
	std::sort(last_visible_pages_.begin(), last_visible_pages_.end());
	last_visible_pages_.erase(
			std::unique(last_visible_pages_.begin(), last_visible_pages_.end()),
			last_visible_pages_.end());
	// A manual visible-set update supersedes any in-flight args readback. Invalidate the
	// request-time pairing so consume_args_readback() ignores a stale readback that arrives
	// later and would otherwise re-promote pages that left draw_pages().
	last_remaining_count_at_request_ = 0;
	first_pass_pages_at_request_.clear();
}

bool LodCullPass::run(RenderingDevice *rd, LodPool &pool, HizPass *hiz,
		const Projection &view_proj, int page_count, int total_page_count,
		int first_pass_count) {
	const auto t0 = std::chrono::steady_clock::now();
	if (!rd || !rd_ || !pipeline_.is_valid() || !stats_readback_.is_valid() ||
			!args_readback_.is_valid()) {
		return false;
	}
	if (page_count <= 0) return false;
	if (!ensure_uniform_set(rd, pool, hiz)) return false;

	if (stats_readback_->take_fresh()) {
		last_drawn_ = stats_readback_->as_i32();
		last_total_ = last_total_at_request_;
		if (last_total_ > 0) last_drawn_ += last_first_pass_count_at_request_;
	}
	if (args_readback_->take_fresh()) consume_args_readback();

	// Stats clear must be recorded before the compute list opens (device-level command).
	PackedByteArray zero;
	zero.resize(4);
	zero.fill(0);
	rd->buffer_update(stats_, 0, 4, zero);

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);

	PackedByteArray pc;
	pc.resize(80); // mat4 + ivec4 params; the shader derives frustum planes from view_proj
	float *f = reinterpret_cast<float *>(pc.ptrw());
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			f[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
	int32_t *ip = reinterpret_cast<int32_t *>(pc.ptrw() + 64);
	ip[0] = page_count;
	ip[1] = HizPass::kSize;
	ip[2] = hiz->mip_count();
	ip[3] = 0;

	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (static_cast<uint32_t>(page_count) + 63u) / 64u, 1, 1);
	rd->compute_list_end();

	// Async stats readback for the HUD's culled ratio. Never synchronous. Record the whole
	// candidate total and the first-pass count paired with the request so a later readback
	// is divided by the right denominator.
	if (stats_readback_->request(rd, stats_, 0, 4)) {
		last_total_at_request_ = total_page_count;
		last_first_pass_count_at_request_ = first_pass_count;
	}
	// Async args readback: the CPU learns which remaining pages survived the cull a few
	// frames later. Paired with the first-pass page snapshot from the same request so
	// last_visible_pages() is the exact union of that frame's two passes. A zero
	// first-pass count (debug probe / single-pass cull) must snapshot an empty first-pass
	// list so stale first-pass pages from a normal temporal frame are not folded in.
	if (args_readback_->request(rd, pool.args_buffer(), 0,
				static_cast<uint32_t>(page_count) * 20u)) {
		if (first_pass_count <= 0) {
			first_pass_pages_at_request_.clear();
		} else {
			first_pass_pages_at_request_ = first_pass_pages_;
		}
		last_remaining_count_at_request_ = page_count;
	}
	last_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return true;
}
