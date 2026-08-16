#include "render/brick_gen_pass.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

Ref<RDUniform> storage(int binding, RID rid) {
	Ref<RDUniform> u;
	u.instantiate();
	u->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u->set_binding(binding);
	u->add_id(rid);
	return u;
}

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

} // namespace

BrickGenPass::~BrickGenPass() {
	teardown();
}

bool BrickGenPass::initialize(RenderingDevice *rd, const GpuAtlas &atlas) {
	teardown();
	rd_ = rd;
	atlas_bricks_ = atlas.config().atlas_bricks;

	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/brick_gen.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("BrickGenPass: shader load failed: ", err.c_str());
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
		UtilityFunctions::printerr("BrickGenPass: ", compile_err);
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

	// The atlas buffers never change identity, so the uniform set is built once.
	Ref<RDUniform> img[5];
	const RID images[5] = {atlas.sdf_atlas(), atlas.mat_atlas(), atlas.mip_atlas(0),
			atlas.mip_atlas(1), atlas.mip_atlas(2)};
	Array uniforms;
	for (int i = 0; i < 5; i++) {
		img[i].instantiate();
		img[i]->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
		img[i]->set_binding(i);
		img[i]->add_id(images[i]);
		uniforms.push_back(img[i]);
	}
	uniforms.push_back(storage(5, atlas.palette()));
	uniforms.push_back(storage(6, atlas.jobs()));
	uniforms.push_back(storage(7, atlas.op_pool()));
	uniforms.push_back(storage(8, atlas.volumes().sdf_buffer()));
	uniforms.push_back(storage(9, atlas.volumes().mat_buffer()));
	uset_ = rd->uniform_set_create(uniforms, shader_, 0);
	if (!uset_.is_valid()) {
		UtilityFunctions::printerr("BrickGenPass: uniform set creation failed");
		teardown();
		return false;
	}
	return true;
}

void BrickGenPass::teardown() {
	if (!rd_) return;
	// M1's documented order: uniform sets first (freeing a shader cascades to its
	// pipelines and referencing sets), then pipeline, then shader.
	free_if_valid(rd_, uset_);
	free_if_valid(rd_, pipeline_);
	free_if_valid(rd_, shader_);
	rd_ = nullptr;
}

void BrickGenPass::dispatch(RenderingDevice *rd, int64_t list, const GpuAtlas &atlas) {
	if (!pipeline_.is_valid()) return;
	PackedByteArray pc;
	pc.resize(16);
	int32_t *p = reinterpret_cast<int32_t *>(pc.ptrw());
	p[0] = atlas_bricks_.x; p[1] = atlas_bricks_.y; p[2] = atlas_bricks_.z; p[3] = 0;
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch_indirect(list, atlas.dispatch_args(), 0);
}
