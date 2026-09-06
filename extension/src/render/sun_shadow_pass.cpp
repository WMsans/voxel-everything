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
	tf->set_array_layers(kCascades);
	tf->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	map_ = rd_->texture_create(tf, tv, {});
	if (!map_.is_valid()) {
		UtilityFunctions::printerr("SunShadowPass: shadow map array creation failed");
		teardown();
		return false;
	}

	// One framebuffer per layer. A shared slice is what lets cascade 0's frequent rebuilds
	// be isolated from cascade 2's expensive one; a single atlased framebuffer would force
	// them to rebuild together and throw the amortization away.
	for (int i = 0; i < kCascades; i++) {
		c_[i].slice = rd_->texture_create_shared_from_slice(tv, map_, i, 0);
		if (!c_[i].slice.is_valid()) {
			UtilityFunctions::printerr("SunShadowPass: cascade slice ", i, " failed");
			teardown();
			return false;
		}
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
		for (int i = 0; i < kCascades; i++) {
			for (RID *r : {&c_[i].framebuffer, &c_[i].slice})
				if (r->is_valid()) rd_->free_rid(*r);
			c_[i] = Cascade{};
		}
		for (RID *r : {&uset_, &pipeline_, &shader_, &map_})
			if (r->is_valid()) rd_->free_rid(*r);
	}
	uset_ = RID();
	pipeline_ = RID();
	shader_ = RID();
	map_ = RID();
	key_quads_ = RID();
	key_page_chunk_ = RID();
	key_chunks_ = RID();
	rd_ = nullptr;
}

void SunShadowPass::mark_dirty() {
	// A page appearing or leaving can change any cascade, so all of them are dirtied. The
	// per-cascade throttle is what keeps that from costing three full rasters.
	for (int i = 0; i < kCascades; i++) {
		c_[i].dirty = true;
		c_[i].frames_since = 0;
	}
}

bool SunShadowPass::ensure_pipeline(RenderingDevice *rd) {
	if (c_[0].framebuffer.is_valid() && pipeline_.is_valid()) return true;
	for (int i = 0; i < kCascades; i++) {
		if (c_[i].framebuffer.is_valid()) rd->free_rid(c_[i].framebuffer);
		c_[i].framebuffer = rd->framebuffer_create(Array::make(c_[i].slice));
		if (!c_[i].framebuffer.is_valid()) return false;
	}
	const int64_t format = rd->framebuffer_get_format(c_[0].framebuffer);

	Ref<RDPipelineRasterizationState> rs;
	rs.instantiate();
	// NO CULLING, and no borrowed front-face convention.
	//
	// Winding is a property of the PROJECTION, not of the geometry: this pass inherited
	// LodRasterPass::front_face_clockwise(), which was measured against the camera's
	// reverse-Z perspective, and the sun's ortho has the opposite handedness. Front and back
	// therefore swapped here, and the pass culled precisely the up-facing terrain quads it
	// exists to record. What survived was the skirt curtains, which lod_append_skirts emits
	// twice with opposite winding -- so the map held a wireframe of chunk boundaries and
	// almost none of the ground, which is why its shadows had the wrong shape.
	//
	// Culling is only ever an optimisation for a depth-only pass. Letting GREATER_OR_EQUAL
	// keep whichever surface is nearest the sun is what a shadow map means, it is correct
	// under any projection, and it cannot be silently inverted by a matrix change again.
	// The cost is bounded: both faces instead of one, but over the cut rather than the whole
	// resident set (2313 -> 1687 pages on the demo view), on a depth-only target that
	// rebuilds at most once every kMinFrames.
	rs->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
	Ref<RDPipelineMultisampleState> ms;
	ms.instantiate();
	Ref<RDPipelineDepthStencilState> ds;
	ds.instantiate();
	ds->set_enable_depth_test(true);
	ds->set_enable_depth_write(true);
	// Reverse-Z (near the sun = 1): GREATER_OR_EQUAL keeps the surface NEAREST the sun, which
	// is what a shadow map means. An overhang's roof therefore wins over its own floor, and
	// two chunks that overlap under a low sun resolve by geometry rather than by draw order.
	//
	// It was briefly ALWAYS, to let a coarsest-first page list end with the finest description
	// of each texel. That was a workaround for the page list carrying several LoD levels of
	// the same ground at once; LodSystem::prepare_shadow_raster now sends a cut instead, so
	// there is one surface per texel and the honest test is back.
	ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
	Ref<RDPipelineColorBlendState> cb;
	cb.instantiate();
	cb->set_attachments(Array());
	pipeline_ = rd->render_pipeline_create(shader_, format, RenderingDevice::INVALID_ID,
			RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	return pipeline_.is_valid();
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

// THE rebuild rule, in one place. The projection moving means what is stored no longer
// describes what will be sampled: rebuild now rather than at the throttle's convenience.
// kMinFrames exists to damp pages coming and going; it must not make a day/night sweep lag
// twelve frames behind the sun, nor leave a cascade a texel behind the camera it follows.
//
// Comparing the whole matrix is affordable precisely because sun_ortho_sphere() snaps: an
// unsnapped camera-following fit differs on every frame the camera moves at all, and this
// test would degenerate into a full 2048^2 pass over the whole cut, every frame.
bool SunShadowPass::should_rebuild(const Cascade &c, const ve::SunOrtho &ortho,
		bool force) const {
	if (!is_valid() || !ortho.valid) return false;
	const bool projection_moved = c.rebuilds > 0 &&
			std::memcmp(c.view_proj, ortho.view_proj, sizeof(c.view_proj)) != 0;
	if (force || projection_moved) return true;
	return c.dirty && c.frames_since >= kMinFrames;
}

bool SunShadowPass::needs_rebuild(int cascade, const ve::SunOrtho &ortho) const {
	return should_rebuild(c_[clamp_index(cascade)], ortho, false);
}

bool SunShadowPass::build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster,
		int cascade, const ve::SunOrtho &ortho, bool force) {
	Cascade &c = c_[clamp_index(cascade)];
	c.frames_since++;
	if (!should_rebuild(c, ortho, force)) return false;
	const std::vector<LodRasterPass::PageDraw> &pages = raster.draw_pages();
	if (pages.empty()) return false;
	if (!raster.prepare_index_array(rd, pool)) return false;
	if (!ensure_pipeline(rd) || !ensure_uniform_set(rd, pool)) return false;
	// The shadow pass and camera pass share this indirect-argument buffer. Upload the full
	// drawable set before recording either draw list; the camera cull must not erase it first.
	pool.upload_draw_args(pages);
	const int64_t dl = rd->draw_list_begin(c.framebuffer, RenderingDevice::DRAW_CLEAR_DEPTH,
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
	std::memcpy(c.view_proj, ortho.view_proj, sizeof(c.view_proj));
	c.texel_world = ortho.texel_world;
	c.depth_range = ortho.depth_range;
	c.dirty = false;
	c.frames_since = 0;
	c.last_pages = static_cast<int>(pages.size());
	c.rebuilds++;
	return true;
}
