#include "render/island_cull_pass.h"
#include "render/island_atlas.h"
#include "render/shader_loader.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

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

IslandCullPass::~IslandCullPass() {
	teardown();
}

bool IslandCullPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path("res://shaders/island_cull.comp.glsl");
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) {
		UtilityFunctions::printerr("IslandCullPass: load failed: ", err.c_str());
		teardown();
		return false;
	}
	Ref<RDShaderSource> src;
	src.instantiate();
	src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	src->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
	const String cerr = spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE);
	if (!cerr.is_empty()) {
		UtilityFunctions::printerr("IslandCullPass: ", cerr);
		teardown();
		return false;
	}
	shader_ = rd->shader_create_from_spirv(spirv);
	pipeline_ = shader_.is_valid() ? rd->compute_pipeline_create(shader_) : RID();
	if (!pipeline_.is_valid()) {
		UtilityFunctions::printerr("IslandCullPass: pipeline creation failed");
		teardown();
		return false;
	}
	return true;
}

void IslandCullPass::teardown() {
	// Uniform sets first: freeing a shader cascades to its pipelines and referencing sets.
	// The mask is referenced by the set, so it is freed after the set.
	free_if_valid(rd_, uset_);
	free_if_valid(rd_, pipeline_);
	free_if_valid(rd_, shader_);
	free_if_valid(rd_, mask_);
	tiles_x_ = 0;
	tiles_y_ = 0;
	rd_ = nullptr;
}

void IslandCullPass::rebuild(RenderingDevice *rd, const IslandAtlas &atlas, int tx, int ty) {
	// Uniform set first: it references the mask buffer about to be freed.
	if (uset_.is_valid()) rd->free_rid(uset_);
	uset_ = RID();
	if (mask_.is_valid()) rd->free_rid(mask_);
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(tx) * ty * 4);
	zero.fill(0);
	mask_ = rd->storage_buffer_create(static_cast<uint32_t>(zero.size()), zero);
	tiles_x_ = tx;
	tiles_y_ = ty;
	uset_ = rd->uniform_set_create(
			Array::make(storage(0, atlas.desc_buffer()), storage(1, mask_)), shader_, 0);
}

bool IslandCullPass::render(RenderingDevice *rd, const IslandAtlas &atlas,
		const ve::CameraParams &cam, int width, int height, int island_count) {
	if (!rd || !is_valid() || width <= 0 || height <= 0 || island_count <= 0) return false;
	const int tx = (width + kIslandTileSize - 1) / kIslandTileSize;
	const int ty = (height + kIslandTileSize - 1) / kIslandTileSize;
	if (tx != tiles_x_ || ty != tiles_y_ || !uset_.is_valid()) rebuild(rd, atlas, tx, ty);
	if (!uset_.is_valid()) return false;

	// The push constant IS ve::CameraParams, with the cull grid in the three trailing ints.
	// Copying rather than re-deriving is the point: the raymarcher gets the same bytes.
	ve::CameraParams pc = cam;
	pc.dims[3] = island_count;
	pc.region_origin[3] = tx;
	pc.atlas_bricks[3] = ty;
	PackedByteArray b;
	b.resize(sizeof(ve::CameraParams));
	std::memcpy(b.ptrw(), &pc, sizeof(ve::CameraParams));

	// Its own compute list. Godot's RenderingDevice ends a compute list with a full barrier
	// unless told otherwise, so the raymarch list that follows sees the finished mask.
	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, pipeline_);
	rd->compute_list_bind_uniform_set(list, uset_, 0);
	rd->compute_list_set_push_constant(list, b, b.size());
	rd->compute_list_dispatch(list, (tx + 7) / 8, (ty + 7) / 8, 1);
	rd->compute_list_end();
	return true;
}
