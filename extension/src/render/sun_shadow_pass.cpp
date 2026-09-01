#include "render/sun_shadow_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

SunShadowPass::~SunShadowPass() {
	teardown();
}

bool SunShadowPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_format(RenderingDevice::DATA_FORMAT_D32_SFLOAT);
	tf->set_width(kSize);
	tf->set_height(kSize);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	map_ = rd_->texture_create(tf, tv, {});
	if (!map_.is_valid()) {
		UtilityFunctions::printerr("SunShadowPass: shadow map creation failed");
		teardown();
		return false;
	}

	std::string err;
	const String path = ProjectSettings::get_singleton()->globalize_path(
			"res://shaders/lod_shadow.vert.glsl");
	const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
	const std::string vertex = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (vertex.empty()) {
		UtilityFunctions::printerr("SunShadowPass: ", err.c_str());
		teardown();
		return false;
	}
	const String frag_path = ProjectSettings::get_singleton()->globalize_path(
			"res://shaders/lod_shadow.frag.glsl");
	const std::string fragment = ve::strip_shader_annotations(
			ve::load_shader_source(frag_path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (fragment.empty()) {
		UtilityFunctions::printerr("SunShadowPass: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_VERTEX, String(vertex.c_str()));
	src->set_stage_source(RenderingDevice::SHADER_STAGE_FRAGMENT, String(fragment.c_str()));
	Ref<RDShaderSPIRV> spirv = rd_->shader_compile_spirv_from_source(src);
	const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
			spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr("SunShadowPass: ", compile_err);
		teardown();
		return false;
	}
	shader_ = rd_->shader_create_from_spirv(spirv);
	if (!shader_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void SunShadowPass::teardown() {
	if (rd_) {
		for (RID *r : {&uset_, &pipeline_, &framebuffer_, &shader_, &map_}) {
			if (r->is_valid()) rd_->free_rid(*r);
			*r = RID();
		}
	}
	uset_ = RID();
	pipeline_ = RID();
	framebuffer_ = RID();
	shader_ = RID();
	map_ = RID();
	key_quads_ = RID();
	key_page_chunk_ = RID();
	key_chunks_ = RID();
	pipeline_front_face_set_ = false;
	rd_ = nullptr;
	dirty_ = true;
	frames_since_ = 0;
	rebuilds_ = 0;
	std::memset(view_proj_, 0, sizeof(view_proj_));
	texel_world_ = 0.0f;
	depth_range_ = 0.0f;
}

void SunShadowPass::mark_dirty() {
	dirty_ = true;
}

bool SunShadowPass::ensure_pipeline(RenderingDevice *rd, LodRasterPass &raster) {
	if (!map_.is_valid() || !shader_.is_valid()) return false;
	const bool clockwise = raster.front_face_clockwise();
	if (framebuffer_.is_valid() && pipeline_.is_valid() && pipeline_front_face_set_ &&
			pipeline_front_face_clockwise_ == clockwise)
		return true;
	if (pipeline_.is_valid()) rd->free_rid(pipeline_);
	pipeline_ = RID();
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	framebuffer_ = rd->framebuffer_create(Array::make(map_));
	if (!framebuffer_.is_valid()) return false;
	const int64_t format = rd->framebuffer_get_format(framebuffer_);

	Ref<RDPipelineRasterizationState> rs;
	rs.instantiate();
	rs->set_cull_mode(RenderingDevice::POLYGON_CULL_BACK);
	rs->set_front_face(clockwise ? RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE :
			RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE);
	Ref<RDPipelineMultisampleState> ms;
	ms.instantiate();
	Ref<RDPipelineDepthStencilState> ds;
	ds.instantiate();
	ds->set_enable_depth_test(true);
	ds->set_enable_depth_write(true);
	ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	Ref<RDPipelineColorBlendState> cb;
	cb.instantiate();
	cb->set_attachments(Array());
	pipeline_ = rd->render_pipeline_create(shader_, format, RenderingDevice::INVALID_ID,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	if (!pipeline_.is_valid()) return false;
	pipeline_front_face_clockwise_ = clockwise;
	pipeline_front_face_set_ = true;
	return true;
}

bool SunShadowPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool) {
	const RID quads = pool.quad_buffer();
	const RID page_chunk = pool.page_chunk_buffer();
	const RID chunks = pool.chunk_buffer();
	if (uset_.is_valid() && key_quads_ == quads && key_page_chunk_ == page_chunk &&
			key_chunks_ == chunks)
		return true;
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	Ref<RDUniform> u0, u1, u2;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u0->set_binding(0);
	u0->add_id(quads);
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u1->set_binding(1);
	u1->add_id(page_chunk);
	u2.instantiate();
	u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u2->set_binding(2);
	u2->add_id(chunks);
	uset_ = rd->uniform_set_create(Array::make(u0, u1, u2), shader_, 0);
	if (!uset_.is_valid()) return false;
	key_quads_ = quads;
	key_page_chunk_ = page_chunk;
	key_chunks_ = chunks;
	return true;
}

bool SunShadowPass::build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster,
		const ve::SunOrtho &ortho, bool force) {
	frames_since_++;
	if (!is_valid() || !ortho.valid) return false;
	// A sun that moved is not LoD churn. kMinFrames throttles rebuilds caused by pages
	// coming and going; it must not make a day/night sweep lag twelve frames behind the
	// light. Comparing the matrix keeps the policy here rather than in every caller, and
	// leaves the camera-motion invariant intact: camera motion does not change this matrix.
	const bool sun_moved = rebuilds_ > 0 &&
			std::memcmp(view_proj_, ortho.view_proj, sizeof(view_proj_)) != 0;
	if (!force && !sun_moved && (!dirty_ || frames_since_ < kMinFrames)) return false;
	const std::vector<LodRasterPass::PageDraw> &pages = raster.draw_pages();
	if (pages.empty()) return false;
	if (!raster.prepare_index_array(rd, pool)) return false;
	if (!ensure_pipeline(rd, raster) || !ensure_uniform_set(rd, pool)) return false;
	// The shadow pass and camera pass share this indirect-argument buffer. Upload the full
	// drawable set before recording either draw list; the camera cull must not erase it first.
	pool.upload_draw_args(pages);
	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_CLEAR_DEPTH,
			PackedColorArray(), 0.0f);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_bind_index_array(dl, raster.index_array());
	PackedByteArray pc;
	pc.resize(64);
	float *f = reinterpret_cast<float *>(pc.ptrw());
	for (int i = 0; i < 16; i++) f[i] = ortho.view_proj[i];
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw_indirect(dl, true, pool.args_buffer(), 0, static_cast<int>(pages.size()), 20);
	rd->draw_list_end();
	std::memcpy(view_proj_, ortho.view_proj, sizeof(view_proj_));
	texel_world_ = ortho.texel_world;
	depth_range_ = ortho.depth_range;
	dirty_ = false;
	frames_since_ = 0;
	rebuilds_++;
	return true;
}
