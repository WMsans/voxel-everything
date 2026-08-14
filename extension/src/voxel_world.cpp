#include "voxel_world.h"
#include "render/gpu_atlas.h"
#include "render/gpu_world.h"
#include "render/camera_params.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/region_pass.h"
#include "render/shader_loader.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
	ClassDB::bind_method(D_METHOD("set_world_size_bricks", "v"), &VoxelWorld::set_world_size_bricks);
	ClassDB::bind_method(D_METHOD("get_world_size_bricks"), &VoxelWorld::get_world_size_bricks);
	ClassDB::bind_method(D_METHOD("set_atlas_bricks", "v"), &VoxelWorld::set_atlas_bricks);
	ClassDB::bind_method(D_METHOD("get_atlas_bricks"), &VoxelWorld::get_atlas_bricks);
	ClassDB::bind_method(D_METHOD("set_max_region_slots", "v"), &VoxelWorld::set_max_region_slots);
	ClassDB::bind_method(D_METHOD("get_max_region_slots"), &VoxelWorld::get_max_region_slots);
	ClassDB::bind_method(D_METHOD("set_max_brick_jobs", "v"), &VoxelWorld::set_max_brick_jobs);
	ClassDB::bind_method(D_METHOD("get_max_brick_jobs"), &VoxelWorld::get_max_brick_jobs);
	ClassDB::bind_method(D_METHOD("set_world_origin_bricks", "v"), &VoxelWorld::set_world_origin_bricks);
	ClassDB::bind_method(D_METHOD("get_world_origin_bricks"), &VoxelWorld::get_world_origin_bricks);
	ClassDB::bind_method(D_METHOD("set_world_size_regions", "v"), &VoxelWorld::set_world_size_regions);
	ClassDB::bind_method(D_METHOD("get_world_size_regions"), &VoxelWorld::get_world_size_regions);
	ClassDB::bind_method(D_METHOD("set_residency_radius_m", "v"), &VoxelWorld::set_residency_radius_m);
	ClassDB::bind_method(D_METHOD("get_residency_radius_m"), &VoxelWorld::get_residency_radius_m);
	ClassDB::bind_method(D_METHOD("ensure_initialized"), &VoxelWorld::ensure_initialized);
	ClassDB::bind_method(D_METHOD("is_initialized"), &VoxelWorld::is_initialized);
	ClassDB::bind_method(D_METHOD("debug_raymarch_pixel", "origin", "dir"), &VoxelWorld::debug_raymarch_pixel);
	ClassDB::bind_method(D_METHOD("debug_indirection_tex"), &VoxelWorld::debug_indirection_tex);
	ClassDB::bind_method(D_METHOD("debug_sdf_atlas"), &VoxelWorld::debug_sdf_atlas);
	ClassDB::bind_method(D_METHOD("debug_local_rd"), &VoxelWorld::debug_local_rd);
	ClassDB::bind_method(D_METHOD("debug_load_shader", "res_path"), &VoxelWorld::debug_load_shader);
	ClassDB::bind_method(D_METHOD("debug_eval_field", "p", "ops", "op_count"), &VoxelWorld::debug_eval_field);
	ClassDB::bind_method(D_METHOD("debug_init_atlas"), &VoxelWorld::debug_init_atlas);
	ClassDB::bind_method(D_METHOD("debug_teardown_atlas"), &VoxelWorld::debug_teardown_atlas);
	ClassDB::bind_method(D_METHOD("debug_atlas_stats"), &VoxelWorld::debug_atlas_stats);
	ClassDB::bind_method(D_METHOD("debug_reset_frame_counters"), &VoxelWorld::debug_reset_frame_counters);
	ClassDB::bind_method(D_METHOD("debug_set_region_map_entry", "region_index", "region_slot"), &VoxelWorld::debug_set_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_upload_region_ops", "region_slot", "ops", "count"), &VoxelWorld::debug_upload_region_ops);
	ClassDB::bind_method(D_METHOD("debug_brick_has_surface", "brick", "ops", "op_count"), &VoxelWorld::debug_brick_has_surface);
	ClassDB::bind_method(D_METHOD("debug_mark_region", "region", "region_slot", "lo", "hi", "op_count", "force"), &VoxelWorld::debug_mark_region);
	ClassDB::bind_method(D_METHOD("debug_release_region", "region_slot"), &VoxelWorld::debug_release_region);
	ClassDB::bind_method(D_METHOD("debug_jobs"), &VoxelWorld::debug_jobs);
	ClassDB::bind_method(D_METHOD("debug_region_table_slot", "region_slot", "brick"), &VoxelWorld::debug_region_table_slot);
	ClassDB::bind_method(D_METHOD("debug_mat_atlas"), &VoxelWorld::debug_mat_atlas);
	ClassDB::bind_method(D_METHOD("debug_mip_atlas", "level"), &VoxelWorld::debug_mip_atlas);
	ClassDB::bind_method(D_METHOD("debug_region_map"), &VoxelWorld::debug_region_map);
	ClassDB::bind_method(D_METHOD("debug_region_tables"), &VoxelWorld::debug_region_tables);
	ClassDB::bind_method(D_METHOD("debug_free_list"), &VoxelWorld::debug_free_list);
	ClassDB::bind_method(D_METHOD("debug_frame_counters"), &VoxelWorld::debug_frame_counters);
	ClassDB::bind_method(D_METHOD("debug_op_pool"), &VoxelWorld::debug_op_pool);
	ClassDB::bind_method(D_METHOD("debug_op_counts"), &VoxelWorld::debug_op_counts);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_bricks"), "set_world_size_bricks", "get_world_size_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "atlas_bricks"), "set_atlas_bricks", "get_atlas_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_region_slots"), "set_max_region_slots", "get_max_region_slots");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_brick_jobs"), "set_max_brick_jobs", "get_max_brick_jobs");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_origin_bricks"), "set_world_origin_bricks", "get_world_origin_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_regions"), "set_world_size_regions", "get_world_size_regions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "residency_radius_m"), "set_residency_radius_m", "get_residency_radius_m");
}

void VoxelWorld::_ready() {}

VoxelWorld::~VoxelWorld() {}

void VoxelWorld::_exit_tree() {
	// Delete the region pass before the atlas: it holds uniform sets referencing the
	// atlas buffers, and freeing those buffers cascades to referencing sets.
	if (region_pass_) {
		delete region_pass_;
		region_pass_ = nullptr;
	}
	// Delete the atlas before the GpuWorld teardown: its destructor frees every RID it
	// owns on rd(), and the device is still valid at this point.
	if (atlas_) {
		delete atlas_;
		atlas_ = nullptr;
	}
	// Delete the raymarch/composite passes while the device is still valid: their
	// destructors free RIDs on rd(), so they must run before GpuWorld teardown and
	// device destruction. The COMPOSITE pass must go first: its cached uniform set
	// references the raymarch color/hitpos textures, and freeing those textures
	// cascades to referencing uniform sets — tearing down raymarch first would leave
	// the composite's uset_ cascade-freed, and its teardown would then hit an
	// "Attempted to free invalid ID" error.
	if (composite_pass_) {
		delete composite_pass_;
		composite_pass_ = nullptr;
	}
	if (raymarch_pass_) {
		delete raymarch_pass_;
		raymarch_pass_ = nullptr;
	}
	if (gpu_) gpu_->teardown();
	// Full reset: keep the node reusable across remove_child/add_child cycles.
	// Without this, ensure_initialized() early-returns on the stale flag and the
	// compositor path runs with a torn-down GpuWorld (freed RIDs) -> black terrain.
	gpu_.reset();
	world_.reset();
	initialized_ = false;
	if (local_rd_) {
		// Brief used local_rd_->free(); godot-cpp master has no no-arg free() on
		// RenderingDevice (only the macro's static free), so free via memdelete.
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
}

RenderingDevice *VoxelWorld::rd() const {
	return use_local_device_ ? local_rd_ : main_rd_;
}

void VoxelWorld::ensure_initialized() {
	if (initialized_) return;
	if (use_local_device_ && !local_rd_) {
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	} else if (!use_local_device_) {
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	}
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	world_ = std::make_unique<ve::WorldData>(world_size_bricks_.x, world_size_bricks_.y, world_size_bricks_.z);
	ve::AnalyticGenerator gen;
	world_->generate(gen);
	UtilityFunctions::print("VoxelWorld: generated ", world_->active_brick_count(), " bricks");
	gpu_ = std::make_unique<GpuWorld>();
	if (!gpu_->initialize(device, *world_)) {
		gpu_.reset();
		return;
	}
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	// Deviation from brief (documented): composite_pass_ is a raw pointer (incomplete-type
	// rule established in Task 9), so `new` here and `delete` in _exit_tree(), matching
	// raymarch_pass_ and the brief's make_unique intent.
	composite_pass_ = new CompositePass();
	composite_pass_->initialize(device);
	initialized_ = true;
}

RID VoxelWorld::debug_indirection_tex() const { return gpu_ ? gpu_->indirection_tex() : RID(); }
// The M2 atlas shadows the M1 GpuWorld atlas once initialized; the M1 fallback keeps the
// GpuWorld suite (which never initializes atlas_) reading the M1 texture.
RID VoxelWorld::debug_sdf_atlas() const {
	return atlas_ ? atlas_->sdf_atlas() : (gpu_ ? gpu_->sdf_atlas() : RID());
}

// Half-precision to single-precision (normal + subnormal paths).
static float half_to_float(uint16_t v) {
	const uint32_t sign = (v & 0x8000u) << 16;
	const uint32_t exp = (v >> 10) & 0x1F;
	const uint32_t mant = v & 0x3FF;
	if (exp == 0) return (sign ? -1.0f : 1.0f) * mant / 1024.0f / 16384.0f;
	uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

Color VoxelWorld::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	ensure_initialized();
	RenderingDevice *device = rd();
	// Deviation 3: initialized_ is not reset on _exit_tree, so after remove/re-add rd()
	// can be stale/null; guard everything (gpu_ additionally null-checked).
	if (!initialized_ || !device || !gpu_ || !raymarch_pass_) return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	cam.dims[0] = world_size_bricks_.x;
	cam.dims[1] = world_size_bricks_.y;
	cam.dims[2] = world_size_bricks_.z;
	if (!raymarch_pass_->render(device, *gpu_, cam, 1, 1)) return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (data.size() < 8) return Color(1, 0, 1);
	const uint16_t *h = reinterpret_cast<const uint16_t *>(data.ptr());
	return Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
}

String VoxelWorld::debug_load_shader(const String &res_path) const {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String path = ps->globalize_path(res_path);
	const String inc = ps->globalize_path("res://shaders");
	std::string err;
	const std::string code =
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err);
	if (code.empty()) {
		UtilityFunctions::printerr("debug_load_shader: ", err.c_str());
		return String();
	}
	return String(code.c_str());
}

Vector2 VoxelWorld::debug_eval_field(Vector3 p, const PackedByteArray &ops, int op_count) const {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = nullptr;
	if (op_count > 0) {
		if (ops.size() < op_count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_eval_field: op buffer too small");
			return Vector2();
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	const ve::Sample s = ve::eval_field(gen, ptr, op_count, p.x, p.y, p.z);
	return Vector2(s.sdf, static_cast<float>(s.material));
}

ve::WorldBounds VoxelWorld::world_bounds() const {
	ve::WorldBounds b;
	b.origin_bricks = {world_origin_bricks_.x, world_origin_bricks_.y, world_origin_bricks_.z};
	b.size_regions = {world_size_regions_.x, world_size_regions_.y, world_size_regions_.z};
	return b;
}

bool VoxelWorld::debug_init_atlas() {
	if (use_local_device_ && !local_rd_)
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	else if (!use_local_device_ && !main_rd_)
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return false;
	}
	if (!atlas_) atlas_ = new GpuAtlas();
	GpuAtlasConfig cfg;
	cfg.atlas_bricks = {atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z};
	cfg.max_region_slots = max_region_slots_;
	cfg.max_brick_jobs = max_brick_jobs_;
	cfg.bounds = world_bounds();
	if (!atlas_->initialize(device, cfg)) return false;
	if (!region_pass_) region_pass_ = new RegionPass();
	if (!region_pass_->initialize(device, *atlas_)) {
		delete region_pass_;
		region_pass_ = nullptr;
		return false;
	}
	return true;
}

void VoxelWorld::debug_teardown_atlas() {
	// The region pass's uniform sets reference the atlas buffers: tear it down first, or
	// freeing the buffers leaves stale sets that error ("free invalid ID") on the next
	// debug_init_atlas() -> RegionPass::initialize() -> teardown().
	if (region_pass_) region_pass_->teardown();
	if (atlas_) atlas_->teardown();
}

Dictionary VoxelWorld::debug_atlas_stats() {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!atlas_ || !atlas_->is_valid() || !device) return d;
	d["slot_count"] = atlas_->atlas_slot_count();
	d["free_slots"] = atlas_->read_free_count(device);
	d["region_map_entries"] = atlas_->region_map_entries();
	d["job_count"] = atlas_->read_job_count(device);
	d["overflow"] = static_cast<int>(atlas_->read_overflow(device));
	return d;
}

void VoxelWorld::debug_reset_frame_counters() {
	if (atlas_ && rd()) atlas_->reset_frame_counters(rd());
}

void VoxelWorld::debug_set_region_map_entry(int region_index, int region_slot) {
	if (atlas_ && rd()) atlas_->set_region_map_entry(rd(), region_index, region_slot);
}

void VoxelWorld::debug_upload_region_ops(int region_slot, const PackedByteArray &ops, int count) {
	if (!atlas_ || !rd()) return;
	const ve::EditOp *ptr = nullptr;
	if (count > 0) {
		if (ops.size() < count * static_cast<int64_t>(sizeof(ve::EditOp))) {
			UtilityFunctions::printerr("debug_upload_region_ops: op buffer too small");
			return;
		}
		ptr = reinterpret_cast<const ve::EditOp *>(ops.ptr());
	}
	atlas_->upload_region_ops(rd(), region_slot, ptr, count);
}

bool VoxelWorld::debug_brick_has_surface(Vector3i brick, const PackedByteArray &ops,
		int op_count) const {
	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr = op_count > 0 ? reinterpret_cast<const ve::EditOp *>(ops.ptr())
	                                     : nullptr;
	return ve::brick_has_surface(gen, ptr, op_count, {brick.x, brick.y, brick.z});
}

void VoxelWorld::debug_mark_region(Vector3i region, int region_slot, Vector3i lo, Vector3i hi,
		int op_count, bool force) {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !region_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->mark(device, list, {region.x, region.y, region.z}, region_slot,
			{lo.x, lo.y, lo.z}, {hi.x, hi.y, hi.z}, op_count, force);
	device->compute_list_end();
	device->submit();
	device->sync();
}

void VoxelWorld::debug_release_region(int region_slot) {
	RenderingDevice *device = rd();
	if (!device || !region_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->release_region(device, list, region_slot);
	device->compute_list_end();
	device->submit();
	device->sync();
}

PackedInt32Array VoxelWorld::debug_jobs() {
	PackedInt32Array out;
	RenderingDevice *device = rd();
	if (!device || !atlas_) return out;
	const int count = atlas_->read_job_count(device);
	if (count <= 0) return out;
	const PackedByteArray b = device->buffer_get_data(atlas_->jobs(), 0, count * 32);
	out.resize(count * 8);
	memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(count) * 32);
	return out;
}

int VoxelWorld::debug_region_table_slot(int region_slot, Vector3i brick) {
	RenderingDevice *device = rd();
	if (!device || !atlas_) return -1;
	const int bi = ve::WorldBounds::brick_index_in_region({brick.x, brick.y, brick.z});
	const uint32_t offset =
			(static_cast<uint32_t>(region_slot) * ve::kRegionBrickCount + bi) * 4;
	const PackedByteArray b = device->buffer_get_data(atlas_->region_tables(), offset, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

RID VoxelWorld::debug_mat_atlas() const { return atlas_ ? atlas_->mat_atlas() : RID(); }
RID VoxelWorld::debug_mip_atlas(int level) const { return atlas_ ? atlas_->mip_atlas(level) : RID(); }
RID VoxelWorld::debug_region_map() const { return atlas_ ? atlas_->region_map() : RID(); }
RID VoxelWorld::debug_region_tables() const { return atlas_ ? atlas_->region_tables() : RID(); }
RID VoxelWorld::debug_free_list() const { return atlas_ ? atlas_->free_list() : RID(); }
RID VoxelWorld::debug_frame_counters() const { return atlas_ ? atlas_->frame_counters() : RID(); }
RID VoxelWorld::debug_op_pool() const { return atlas_ ? atlas_->op_pool() : RID(); }
RID VoxelWorld::debug_op_counts() const { return atlas_ ? atlas_->op_counts() : RID(); }
