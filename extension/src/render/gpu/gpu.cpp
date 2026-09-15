#include "render/gpu/gpu.h"
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
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <string>
#include <utility>

namespace godot::gpu {

static_assert(static_cast<int>(ve::gpu::UniformType::SamplerWithTexture) ==
		RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
static_assert(static_cast<int>(ve::gpu::UniformType::Image) == RenderingDevice::UNIFORM_TYPE_IMAGE);
static_assert(static_cast<int>(ve::gpu::UniformType::UniformBuffer) ==
		RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
static_assert(static_cast<int>(ve::gpu::UniformType::StorageBuffer) ==
		RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);

bool RdDevice::alive(Kind kind, const RID &id) {
	if (!rd || !id.is_valid()) return false;
	switch (kind) {
		case Kind::UniformSet: return rd->uniform_set_is_valid(id);
		case Kind::Framebuffer: return rd->framebuffer_is_valid(id);
		case Kind::Texture: return rd->texture_is_valid(id);
		case Kind::Pipeline: return rd->compute_pipeline_is_valid(id) || rd->render_pipeline_is_valid(id);
		// No query exists for these, and nothing in this engine frees them by cascade.
		case Kind::IndexArray:
		case Kind::Shader:
		case Kind::Sampler:
		case Kind::Buffer: return true;
	}
	return true;
}

void RdDevice::free(const RID &id) {
	rd->free_rid(id);
}

RID RdDevice::create_uniform_set(const RID &shader, uint32_t set,
		const std::vector<ve::gpu::Uniform<RID>> &uniforms) {
	Array array;
	for (const ve::gpu::Uniform<RID> &u : uniforms) {
		Ref<RDUniform> r;
		r.instantiate();
		r->set_uniform_type(static_cast<RenderingDevice::UniformType>(u.type));
		r->set_binding(static_cast<int32_t>(u.binding));
		for (uint8_t i = 0; i < u.count; i++) r->add_id(u.ids[i]);
		array.push_back(r);
	}
	return rd->uniform_set_create(array, shader, set);
}

RID uniform_set(RenderingDevice *rd, Group &group, const RID &shader, uint32_t set,
		const std::vector<Uniform> &uniforms) {
	RdDevice device{rd};
	return ve::gpu::uniform_set(device, group, shader, set, uniforms);
}

namespace {

// "" with *error set on failure.
std::string load_stage(const String &res_path, const char *defines, std::string *error) {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(res_path);
	const String inc = ps->globalize_path("res://shaders");
	std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), error));
	if (!code.empty() && defines && *defines) code = ve::insert_after_version(code, defines);
	return code;
}

using StageSource = std::pair<RenderingDevice::ShaderStage, const std::string *>;

// Every stage's compile error concatenated into *error ("" on success).
Ref<RDShaderSPIRV> compile_stages(RenderingDevice *rd, std::initializer_list<StageSource> stages,
		String *error) {
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	for (const StageSource &s : stages) src->set_stage_source(s.first, String(s.second->c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	*error = String();
	for (const StageSource &s : stages) *error += spirv->get_stage_compile_error(s.first);
	return spirv;
}

} // namespace

Program compile_compute(RenderingDevice *rd, Group &group, const char *label, const char *file,
		const char *defines) {
	Program p;
	if (!rd) return p;
	std::string err;
	const std::string code = load_stage(String("res://shaders/") + file, defines, &err);
	if (code.empty()) {
		UtilityFunctions::printerr(label, ": ", file, " load failed: ", err.c_str());
		return p;
	}
	String compile_err;
	const Ref<RDShaderSPIRV> spirv =
			compile_stages(rd, {{RenderingDevice::SHADER_STAGE_COMPUTE, &code}}, &compile_err);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr(label, ": ", file, ": ", compile_err);
		return p;
	}
	p.shader = group.add(Kind::Shader, rd->shader_create_from_spirv(spirv));
	if (p.shader.is_valid()) p.pipeline = group.add(Kind::Pipeline, rd->compute_pipeline_create(p.shader));
	if (!p.valid()) UtilityFunctions::printerr(label, ": ", file, ": pipeline creation failed");
	return p;
}

RID compile_raster(RenderingDevice *rd, Group &group, const char *label, const char *vertex_file,
		const char *fragment_file, const char *defines) {
	if (!rd) return RID();
	std::string err;
	const std::string vertex = load_stage(String("res://shaders/") + vertex_file, defines, &err);
	if (vertex.empty()) {
		UtilityFunctions::printerr(label, ": ", vertex_file, " load failed: ", err.c_str());
		return RID();
	}
	const std::string fragment = load_stage(String("res://shaders/") + fragment_file, defines, &err);
	if (fragment.empty()) {
		UtilityFunctions::printerr(label, ": ", fragment_file, " load failed: ", err.c_str());
		return RID();
	}
	String compile_err;
	const Ref<RDShaderSPIRV> spirv = compile_stages(rd,
			{{RenderingDevice::SHADER_STAGE_VERTEX, &vertex},
					{RenderingDevice::SHADER_STAGE_FRAGMENT, &fragment}},
			&compile_err);
	if (!compile_err.is_empty()) {
		UtilityFunctions::printerr(label, ": ", compile_err);
		return RID();
	}
	const RID shader = group.add(Kind::Shader, rd->shader_create_from_spirv(spirv));
	if (!shader.is_valid()) UtilityFunctions::printerr(label, ": shader creation failed");
	return shader;
}

bool compile_check(RenderingDevice *rd, const String &res_path, RenderingDevice::ShaderStage stage,
		String *out_error) {
	std::string err;
	const std::string code = load_stage(res_path, "", &err);
	if (code.empty()) {
		if (out_error) *out_error = res_path + String(": ") + String(err.c_str());
		return false;
	}
	String compile_err;
	compile_stages(rd, {{stage, &code}}, &compile_err);
	if (!compile_err.is_empty()) {
		if (out_error) *out_error = res_path + String(": ") + compile_err;
		return false;
	}
	return true;
}

RID sampler(RenderingDevice *rd, Group &group, RenderingDevice::SamplerFilter filter,
		bool clamp_to_edge) {
	Ref<RDSamplerState> s;
	s.instantiate();
	s->set_min_filter(filter);
	s->set_mag_filter(filter);
	if (clamp_to_edge) {
		s->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		s->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
		s->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	}
	return group.add(Kind::Sampler, rd->sampler_create(s));
}

RID texture(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format, Vector2i size,
		uint32_t usage, const TypedArray<PackedByteArray> &data) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_format(format);
	f->set_width(size.x);
	f->set_height(size.y);
	f->set_usage_bits(usage);
	Ref<RDTextureView> v;
	v.instantiate();
	return group.add(Kind::Texture, rd->texture_create(f, v, data));
}

bool Target::ensure(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format,
		Vector2i size, uint32_t usage, const Color *clear) {
	if (!rd || size.x <= 0 || size.y <= 0) return false;
	if (rid_.is_valid() && size == size_) return true;
	if (rid_.is_valid()) {
		RdDevice device{rd};
		group.free(device, rid_);
	}
	size_ = Vector2i(0, 0);
	rid_ = texture(rd, group, format, size, usage);
	if (!rid_.is_valid()) return false;
	if (clear) rd->texture_clear(rid_, *clear, 0, 1, 0, 1);
	size_ = size;
	return true;
}

RID FramebufferCache::get(RenderingDevice *rd, Group &group, const std::vector<RID> &attachments) {
	if (rid_.is_valid() && attachments == attachments_ && rd->framebuffer_is_valid(rid_)) return rid_;
	release(rd, group);
	Array array;
	for (const RID &a : attachments) array.push_back(a);
	const RID fb = group.add(Kind::Framebuffer, rd->framebuffer_create(array));
	if (!rd->framebuffer_is_valid(fb)) return RID();
	rid_ = fb;
	attachments_ = attachments;
	format_ = rd->framebuffer_get_format(fb);
	return rid_;
}

void FramebufferCache::release(RenderingDevice *rd, Group &group) {
	if (rid_.is_valid()) {
		RdDevice device{rd};
		group.free(device, rid_);
	}
	rid_ = RID();
	attachments_.clear();
}

RID raster_pipeline(RenderingDevice *rd, Group &group, const RID &shader, int64_t fb_format,
		const RasterState &state) {
	Ref<RDPipelineRasterizationState> rs;
	rs.instantiate();
	rs->set_cull_mode(state.cull);
	rs->set_front_face(state.front);
	Ref<RDPipelineMultisampleState> ms;
	ms.instantiate();
	Ref<RDPipelineDepthStencilState> ds;
	ds.instantiate();
	ds->set_enable_depth_test(state.depth_test);
	ds->set_enable_depth_write(state.depth_write);
	ds->set_depth_compare_operator(state.compare);
	Array attachments;
	for (int i = 0; i < state.color_attachments; i++) {
		Ref<RDPipelineColorBlendStateAttachment> a;
		a.instantiate();
		a->set_enable_blend(false);
		attachments.push_back(a);
	}
	Ref<RDPipelineColorBlendState> cb;
	cb.instantiate();
	cb->set_attachments(attachments);
	if (state.logic_or) {
		cb->set_enable_logic_op(true);
		cb->set_logic_op(RenderingDevice::LOGIC_OP_OR);
	}
	return group.add(Kind::Pipeline, rd->render_pipeline_create(shader, fb_format,
			RenderingDevice::INVALID_ID, RenderingDevice::RENDER_PRIMITIVE_TRIANGLES, rs, ms, ds, cb));
}

bool dispatch(RenderingDevice *rd, const RID &pipeline, std::initializer_list<SetSlot> sets,
		const PackedByteArray &push, uint32_t x, uint32_t y, uint32_t z) {
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, pipeline);
	for (const SetSlot &s : sets) rd->compute_list_bind_uniform_set(list, s.set, s.index);
	if (!push.is_empty()) rd->compute_list_set_push_constant(list, push, push.size());
	rd->compute_list_dispatch(list, x, y, z);
	rd->compute_list_end();
	return true;
}

} // namespace godot::gpu
