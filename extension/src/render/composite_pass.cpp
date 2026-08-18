#include "render/composite_pass.h"
#include "render/material_atlas.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state.hpp>
#include <godot_cpp/classes/rd_pipeline_color_blend_state_attachment.hpp>
#include <godot_cpp/classes/rd_pipeline_depth_stencil_state.hpp>
#include <godot_cpp/classes/rd_pipeline_multisample_state.hpp>
#include <godot_cpp/classes/rd_pipeline_rasterization_state.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

CompositePass::~CompositePass() {
	teardown();
}

void CompositePass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	auto load_stage = [&](const char *file, RenderingDevice::ShaderStage stage,
			Ref<RDShaderSource> &src, bool marker) -> bool {
		std::string err;
		const String path = ProjectSettings::get_singleton()->globalize_path(String("res://shaders/") + file);
		const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
		std::string code = ve::strip_shader_annotations(
				ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
		if (code.empty()) {
			UtilityFunctions::printerr("CompositePass: ", err.c_str());
			return false;
		}
		if (marker) {
			// The marker output must not be present in production pipelines: Godot rejects
			// a fragment output mask that does not match the framebuffer's color outputs.
			// The seam-probe shader variant is compiled with SEAM_MARKER so it can write the
			// R8_UINT marker attachment.
			const size_t version_end = code.find('\n', code.find("#version"));
			if (version_end != std::string::npos)
				code.insert(version_end + 1, "#define SEAM_MARKER 1\n");
		}
		src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
		src->set_stage_source(stage, String(code.c_str()));
		return true;
	};
	auto make_shader = [&](bool marker) -> RID {
		Ref<RDShaderSource> src;
		src.instantiate();
		if (!load_stage("composite.vert.glsl", RenderingDevice::SHADER_STAGE_VERTEX, src, marker)) return RID();
		if (!load_stage("composite.frag.glsl", RenderingDevice::SHADER_STAGE_FRAGMENT, src, marker)) return RID();
		Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
		const String compile_err = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_VERTEX) +
				spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_FRAGMENT);
		if (!compile_err.is_empty()) {
			UtilityFunctions::printerr("CompositePass: ", compile_err);
			return RID();
		}
		return rd->shader_create_from_spirv(spirv);
	};
	shader_ = make_shader(false);
	shader_marker_ = make_shader(true);

	Ref<RDSamplerState> sl;
	sl.instantiate();
	sl->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sl->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler_linear_ = rd->sampler_create(sl);
	Ref<RDSamplerState> sn;
	sn.instantiate();
	sn->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sn->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_nearest_ = rd->sampler_create(sn);
}

void CompositePass::release_targets() {
	if (rd_ && framebuffer_.is_valid()) rd_->free_rid(framebuffer_);
	framebuffer_ = RID();
	fb_color_ = RID();
	fb_depth_ = RID();
	fb_marker_ = RID();
}

void CompositePass::teardown() {
	if (!rd_) return;
	// Free order matters on Godot 4.7.1's RenderingDevice (empirically established in
	// Task 10): freeing a shader cascades to its pipelines, and a uniform set references
	// the shader, so the uniform set must be freed BEFORE the pipeline/shader or
	// free_rid hits an already-cascade-freed ID. The brief freed shader before pipeline;
	// the task's ordering (uset -> pipeline -> shader -> targets) is the correct one on
	// this engine.
	for (RID *r : {&uset_, &pipeline_, &shader_, &shader_marker_,
				&sampler_linear_, &sampler_nearest_, &framebuffer_}) {
		if (r->is_valid()) rd_->free_rid(*r);
		*r = RID();
	}
	uset_shader_ = RID();
	uset_src_color_ = RID();
	uset_src_hitpos_ = RID();
	uset_material_albedo_ = RID();
	uset_material_surface_ = RID();
	uset_material_sampler_ = RID();
	fb_marker_ = RID();
	rd_ = nullptr;
}

bool CompositePass::ensure_pipeline(RenderingDevice *rd, RID dst_color, RID dst_depth, RID marker) {
	const bool want_marker = marker.is_valid();
	const RID shader = want_marker ? shader_marker_ : shader_;
	if (!shader.is_valid()) return false;
	if (pipeline_.is_valid() && framebuffer_.is_valid() &&
			dst_color == fb_color_ && dst_depth == fb_depth_ && marker == fb_marker_ &&
			pipeline_marker_ == want_marker) {
		return true;
	}
	// Master-API deviation (documented): the brief builds RDAttachmentFormat/RDFramebufferPass
	// objects and passes them to framebuffer_format_create, but on godot-cpp master
	// RDFramebufferPass::set_color_attachments takes attachment INDICES (PackedInt32Array)
	// and the single-pass framebuffer_format_create takes the TypedArray<Ref<RDAttachmentFormat>>
	// directly (depth is auto-detected by the DEPTH_STENCIL_ATTACHMENT usage bit — verified
	// against engine source). More importantly, the pipeline's framebuffer format must be the
	// EXACT format ID the framebuffer derives from the destination textures (usage flags are
	// part of the framebuffer-format cache key, and the draw path ERR_FAILs on any pipeline/
	// framebuffer format mismatch), so we create the framebuffer from the scene textures first
	// and read its format via framebuffer_get_format for the pipeline. This still satisfies the
	// brief's "build the framebuffer format lazily from the destination textures' formats".
	if (framebuffer_.is_valid()) rd->free_rid(framebuffer_);
	const Array attachments = want_marker ?
			Array::make(dst_color, marker, dst_depth) : Array::make(dst_color, dst_depth);
	framebuffer_ = rd->framebuffer_create(attachments);
	fb_color_ = dst_color;
	fb_depth_ = dst_depth;
	fb_marker_ = marker;
	if (!framebuffer_.is_valid()) return false;
	fb_format_ = rd->framebuffer_get_format(framebuffer_);

	if (!pipeline_.is_valid() || pipeline_marker_ != want_marker) {
		if (pipeline_.is_valid()) rd->free_rid(pipeline_);
		pipeline_ = RID();
		Ref<RDPipelineRasterizationState> rs;
		rs.instantiate();
		rs->set_cull_mode(RenderingDevice::POLYGON_CULL_DISABLED);
		Ref<RDPipelineMultisampleState> ms;
		ms.instantiate();
		Ref<RDPipelineDepthStencilState> ds;
		ds.instantiate();
		ds->set_enable_depth_test(true);
		ds->set_enable_depth_write(true);
		// Reverse-Z: Godot 4.7 renders with near=1.0/far=0.0 (the scene projection is
		// depth-corrected), so GREATER_OR_EQUAL both writes our terrain depth where it is
		// nearer than the current buffer (opaque pre-pass depth) and leaves nearer scene
		// geometry untouched. The brief's COMPARE_OP_ALWAYS would clobber the cube's
		// pre-pass depth with farther terrain depth, culling the cube in its own pass.
		ds->set_depth_compare_operator(RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL);
		Ref<RDPipelineColorBlendStateAttachment> att;
		att.instantiate();
		att->set_enable_blend(false);
		Ref<RDPipelineColorBlendState> cb;
		cb.instantiate();
		if (want_marker) {
			Ref<RDPipelineColorBlendStateAttachment> att_marker;
			att_marker.instantiate();
			att_marker->set_enable_blend(false);
			cb->set_attachments(Array::make(att, att_marker));
		} else {
			cb->set_attachments(Array::make(att));
		}
		pipeline_marker_ = want_marker;
		// Master-API deviation: the brief created an empty vertex format via
		// vertex_format_create(Array()) and passed it to render_pipeline_create, but on
		// master an empty vertex format is a VALID format that "expects vertices" (the draw
		// path ERR_FAILs with "No vertex array was bound"). Vertexless fullscreen-triangle
		// pipelines must be created with RenderingDevice::INVALID_ID as the vertex format —
		// exactly what Godot's own fullscreen passes do (verified: copy_effects.cpp passes
		// RD::INVALID_ID). gl_VertexIndex then supplies the 3 procedural vertices.
		pipeline_ = rd->render_pipeline_create(shader, fb_format_, RenderingDevice::INVALID_ID,
				RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb);
	}
	return pipeline_.is_valid() && framebuffer_.is_valid();
}

void CompositePass::draw(RenderingDevice *rd, RID dst_color, RID dst_depth,
		RID src_color, RID src_hitpos, const Projection &view_proj,
		const MaterialAtlas &materials, const float cam_pos[3],
		float fade_start, float fade_end, RID marker) {
	const RID shader = marker.is_valid() ? shader_marker_ : shader_;
	if (!shader.is_valid()) return;
	if (!ensure_pipeline(rd, dst_color, dst_depth, marker)) return;

	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0);
	u0->add_id(sampler_linear_);
	u0->add_id(src_color);
	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u1->set_binding(1);
	u1->add_id(sampler_nearest_);
	u1->add_id(src_hitpos);
	Ref<RDUniform> u2;
	u2.instantiate();
	u2->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u2->set_binding(2);
	u2->add_id(materials.sampler());
	u2->add_id(materials.albedo_array());
	Ref<RDUniform> u3;
	u3.instantiate();
	u3->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u3->set_binding(3);
	u3->add_id(materials.sampler());
	u3->add_id(materials.surface_array());
	// Cache the uniform set (RD API contract: free uniform-set RIDs when done — creating
	// one per draw leaks a RID every frame). Rebuild only when the bound source textures
	// change; free the old set before recreating (it references the shader, not the
	// textures, so it is cascade-safe to free here).
	if (uset_.is_valid() && uset_shader_ == shader &&
			src_color == uset_src_color_ && src_hitpos == uset_src_hitpos_ &&
			materials.albedo_array() == uset_material_albedo_ &&
			materials.surface_array() == uset_material_surface_ &&
			materials.sampler() == uset_material_sampler_) {
		// Reuse cached set.
	} else {
		if (uset_.is_valid()) rd->free_rid(uset_);
		uset_ = RID();
		uset_ = rd->uniform_set_create(Array::make(u0, u1, u2, u3), shader, 0);
		uset_shader_ = shader;
		uset_src_color_ = src_color;
		uset_src_hitpos_ = src_hitpos;
		uset_material_albedo_ = materials.albedo_array();
		uset_material_surface_ = materials.surface_array();
		uset_material_sampler_ = materials.sampler();
	}
	if (!uset_.is_valid()) return;

	PackedByteArray pc;
	pc.resize(96);
	{
		float *f = reinterpret_cast<float *>(pc.ptrw());
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				f[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
		// std430 push block: mat4 view_proj is floats 0..15, vec4 cam starts at float 16,
		// vec4 fade at float 20. Index by float, not byte (plan errata 3).
		f[16] = cam_pos[0];
		f[17] = cam_pos[1];
		f[18] = cam_pos[2];
		f[19] = fade_start;
		f[20] = fade_end;
	}

	// Master-API deviation (documented): the brief's 5-arg draw_list_begin(initial/final
	// color+depth actions) is the pre-4.3 API. On godot-cpp master draw_list_begin takes a
	// single DrawFlags bitfield; DRAW_DEFAULT_ALL (=0) means no clears = LOAD color, STORE
	// color, LOAD depth, STORE depth — exactly the brief's intended actions (we must not
	// clear: the scene color/depth contents matter).
	const int64_t dl = rd->draw_list_begin(framebuffer_, RenderingDevice::DRAW_DEFAULT_ALL);
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, uset_, 0);
	rd->draw_list_set_push_constant(dl, pc, pc.size());
	rd->draw_list_draw(dl, false, 1, 3);
	rd->draw_list_end();
}
