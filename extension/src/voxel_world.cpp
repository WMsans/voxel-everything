#include "voxel_world.h"
#include "render/gpu_atlas.h"
#include "render/camera_params.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/region_pass.h"
#include "render/brick_gen_pass.h"
#include "render/world_streamer.h"
#include "render/shader_loader.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/brick_mip.h"
#include "world/raycast.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>
#include <cstring>
#include <algorithm>

using namespace godot;

void VoxelWorld::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_use_local_device", "v"), &VoxelWorld::set_use_local_device);
	ClassDB::bind_method(D_METHOD("get_use_local_device"), &VoxelWorld::get_use_local_device);
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
	ClassDB::bind_method(D_METHOD("debug_raymarch_probe", "origin", "dir"), &VoxelWorld::debug_raymarch_probe);
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
	ClassDB::bind_method(D_METHOD("debug_generate_pending"), &VoxelWorld::debug_generate_pending);
	ClassDB::bind_method(D_METHOD("debug_brick_diff", "brick", "region_slot", "ops", "op_count"), &VoxelWorld::debug_brick_diff);
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
	ClassDB::bind_method(D_METHOD("debug_stream_frame", "cam"), &VoxelWorld::debug_stream_frame);
	ClassDB::bind_method(D_METHOD("debug_stream_stats"), &VoxelWorld::debug_stream_stats);
	ClassDB::bind_method(D_METHOD("debug_slot_of_region", "region"), &VoxelWorld::debug_slot_of_region);
	ClassDB::bind_method(D_METHOD("debug_region_map_entry", "region"), &VoxelWorld::debug_region_map_entry);
	ClassDB::bind_method(D_METHOD("debug_region_map_consistent"), &VoxelWorld::debug_region_map_consistent);
	ClassDB::bind_method(D_METHOD("debug_raycast", "origin", "dir"), &VoxelWorld::debug_raycast);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "use_local_device"), "set_use_local_device", "get_use_local_device");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "atlas_bricks"), "set_atlas_bricks", "get_atlas_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_region_slots"), "set_max_region_slots", "get_max_region_slots");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_brick_jobs"), "set_max_brick_jobs", "get_max_brick_jobs");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_origin_bricks"), "set_world_origin_bricks", "get_world_origin_bricks");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3I, "world_size_regions"), "set_world_size_regions", "get_world_size_regions");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "residency_radius_m"), "set_residency_radius_m", "get_residency_radius_m");
}

void VoxelWorld::_ready() {}

VoxelWorld::~VoxelWorld() {}

void VoxelWorld::teardown_gpu() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order).
	if (composite_pass_) { delete composite_pass_; composite_pass_ = nullptr; }
	if (raymarch_pass_) { delete raymarch_pass_; raymarch_pass_ = nullptr; }
	if (gen_pass_) { delete gen_pass_; gen_pass_ = nullptr; }
	if (region_pass_) { delete region_pass_; region_pass_ = nullptr; }
	if (streamer_) { delete streamer_; streamer_ = nullptr; }
	if (residency_) { residency_->clear(); } // slot assignments are meaningless pre-atlas
	if (atlas_) { delete atlas_; atlas_ = nullptr; }
	initialized_ = false;
}

void VoxelWorld::_exit_tree() {
	teardown_gpu();
	if (residency_) { delete residency_; residency_ = nullptr; }
	if (edit_log_) { delete edit_log_; edit_log_ = nullptr; }
	pending_edits_.clear();
	overflow_seen_ = 0;
	if (local_rd_) {
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
	main_rd_ = nullptr;
}

void VoxelWorld::ensure_initialized() {
	if (initialized_) return;
	if (use_local_device_ && !local_rd_) {
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	} else if (!use_local_device_ && !main_rd_) {
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	}
	RenderingDevice *device = rd();
	if (!device) {
		UtilityFunctions::printerr("VoxelWorld: no RenderingDevice");
		return;
	}
	atlas_ = new GpuAtlas();
	GpuAtlasConfig cfg;
	cfg.atlas_bricks = {atlas_bricks_.x, atlas_bricks_.y, atlas_bricks_.z};
	cfg.max_region_slots = max_region_slots_;
	cfg.max_brick_jobs = max_brick_jobs_;
	cfg.bounds = world_bounds();
	if (!atlas_->initialize(device, cfg)) { delete atlas_; atlas_ = nullptr; return; }
	region_pass_ = new RegionPass();
	if (!region_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	gen_pass_ = new BrickGenPass();
	if (!gen_pass_->initialize(device, *atlas_)) { teardown_gpu(); return; }
	if (!edit_log_) edit_log_ = new ve::EditLog(world_bounds());
	if (!residency_) {
		ve::ResidencyConfig rcfg;
		rcfg.bounds = world_bounds();
		rcfg.radius_m = residency_radius_m_;
		rcfg.max_region_slots = max_region_slots_;
		residency_ = new ve::RegionResidency(rcfg);
	}
	streamer_ = new WorldStreamer();
	streamer_->initialize(residency_, edit_log_, &edit_mutex_, &pending_edits_, atlas_,
			region_pass_, gen_pass_);
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	composite_pass_ = new CompositePass();
	composite_pass_->initialize(device);
	initialized_ = true;
}

ve::EditLog::AppendResult VoxelWorld::append_edit(const ve::EditOp &op) {
	std::lock_guard<std::mutex> lock(edit_mutex_);
	if (!edit_log_) return {};
	ve::EditLog::AppendResult r = edit_log_->append(op);
	if (!r.rejected.empty()) {
		UtilityFunctions::printerr("VoxelWorld: region op list full, op rejected (",
				r.rejected[0].x, ", ", r.rejected[0].y, ", ", r.rejected[0].z,
				") — spec §8 fail-soft");
	}
	pending_edits_.push_back({op, r});
	return r;
}

RenderingDevice *VoxelWorld::rd() const {
	return use_local_device_ ? local_rd_ : main_rd_;
}

ve::WorldBounds VoxelWorld::world_bounds() const {
	ve::WorldBounds b;
	b.origin_bricks = {world_origin_bricks_.x, world_origin_bricks_.y, world_origin_bricks_.z};
	b.size_regions = {world_size_regions_.x, world_size_regions_.y, world_size_regions_.z};
	return b;
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
	if (!initialized_ || !device || !atlas_ || !raymarch_pass_) return Color(1, 0, 1);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 ro = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.region_origin[0] = ro.x; cam.region_origin[1] = ro.y; cam.region_origin[2] = ro.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, cam, 1, 1, kNoEdit)) return Color(1, 0, 1);
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (data.size() < 8) return Color(1, 0, 1);
	const uint16_t *h = reinterpret_cast<const uint16_t *>(data.ptr());
	return Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
}

Dictionary VoxelWorld::debug_raymarch_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !raymarch_pass_) return d;
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, 0, 1, 0);
	const ve::WorldBounds wb = world_bounds();
	const ve::IVec3 rorig = wb.origin_regions();
	cam.dims[0] = world_size_regions_.x; cam.dims[1] = world_size_regions_.y;
	cam.dims[2] = world_size_regions_.z;
	cam.region_origin[0] = rorig.x; cam.region_origin[1] = rorig.y; cam.region_origin[2] = rorig.z;
	cam.atlas_bricks[0] = atlas_bricks_.x; cam.atlas_bricks[1] = atlas_bricks_.y;
	cam.atlas_bricks[2] = atlas_bricks_.z;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!raymarch_pass_->render(device, *atlas_, cam, 1, 1, kNoEdit)) return d;
	device->submit();
	device->sync();
	const PackedByteArray hp = device->texture_get_data(raymarch_pass_->hitpos_texture(), 0);
	const PackedByteArray col = device->texture_get_data(raymarch_pass_->color_texture(), 0);
	if (hp.size() < 16 || col.size() < 8) return d;
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	const uint16_t *h = reinterpret_cast<const uint16_t *>(col.ptr());
	d["color"] = Color(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0);
	if (hf[3] < 0.5f) return d; // sky miss
	d["hit"] = true;
	const ve::IVec3 brick = ve::WorldBounds::brick_of_point(hf[0], hf[1], hf[2]);
	d["brick"] = Vector3i(brick.x, brick.y, brick.z);
	// Reproduce the shader's lookups on the CPU to report what the mips said there.
	const ve::IVec3 rs_region = ve::WorldBounds::region_of_brick(brick);
	const int rslot = debug_region_map_entry(Vector3i(rs_region.x, rs_region.y, rs_region.z));
	if (rslot < 0) return d;
	const int slot = debug_region_table_slot(rslot, Vector3i(brick.x, brick.y, brick.z));
	if (slot < 0) return d;
	const float lx = hf[0] - brick.x * ve::kBrickSize;
	const float ly = hf[1] - brick.y * ve::kBrickSize;
	const float lz = hf[2] - brick.z * ve::kBrickSize;
	const int cx = std::min(7, std::max(0, static_cast<int>(lx / ve::kVoxelSize) / 2));
	const int cy = std::min(7, std::max(0, static_cast<int>(ly / ve::kVoxelSize) / 2));
	const int cz = std::min(7, std::max(0, static_cast<int>(lz / ve::kVoxelSize) / 2));
	d["cell8"] = Vector3i(cx, cy, cz);
	const ve::IVec3 abv = atlas_->config().atlas_bricks;
	const ve::IVec3 cell{slot % abv.x, (slot / abv.x) % abv.y, slot / (abv.x * abv.y)};
	const PackedByteArray m2 = device->texture_get_data(atlas_->mip_atlas(0), 0);
	const PackedByteArray m8 = device->texture_get_data(atlas_->mip_atlas(2), 0);
	{
		const int w = abv.x * 2, hh = abv.y * 2;
		uint8_t mn = 255, mx = 0;
		for (int z = 0; z < 2; z++)
			for (int y = 0; y < 2; y++)
				for (int x = 0; x < 2; x++) {
					const int64_t o = (static_cast<int64_t>(cell.x * 2 + x) +
							(cell.y * 2 + y) * w + (cell.z * 2 + z) * w * hh) * 2;
					mn = std::min(mn, m2[o]);
					mx = std::max(mx, m2[o + 1]);
				}
		d["brick_surface"] = mn <= ve::kEncodedZero && mx >= ve::kEncodedZero;
	}
	{
		const int w = abv.x * 8, hh = abv.y * 8;
		const int64_t o = (static_cast<int64_t>(cell.x * 8 + cx) + (cell.y * 8 + cy) * w +
				(cell.z * 8 + cz) * static_cast<int64_t>(w) * hh) * 2;
		d["cell8_surface"] = m8[o] <= ve::kEncodedZero && m8[o + 1] >= ve::kEncodedZero;
	}
	return d;
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

bool VoxelWorld::debug_init_atlas() {
	ensure_initialized();
	return atlas_ && atlas_->is_valid();
}

void VoxelWorld::debug_teardown_atlas() {
	teardown_gpu();
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

void VoxelWorld::debug_generate_pending() {
	RenderingDevice *device = rd();
	if (!device || !atlas_ || !region_pass_ || !gen_pass_) return;
	const int64_t list = device->compute_list_begin();
	region_pass_->write_dispatch_args(device, list);
	device->compute_list_add_barrier(list);
	gen_pass_->dispatch(device, list, *atlas_);
	device->compute_list_end();
	device->submit();
	device->sync();
}

Dictionary VoxelWorld::debug_brick_diff(Vector3i brick, int region_slot,
		const PackedByteArray &ops, int op_count) {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!device || !atlas_) return d;
	const ve::IVec3 b{brick.x, brick.y, brick.z};
	const int slot = debug_region_table_slot(region_slot, brick);
	d["slot"] = slot;
	if (slot < 0) return d;

	ve::AnalyticGenerator gen;
	const ve::EditOp *ptr =
			op_count > 0 ? reinterpret_cast<const ve::EditOp *>(ops.ptr()) : nullptr;
	ve::BrickEval ref{};
	ve::eval_brick(gen, ptr, op_count, b, &ref);

	const ve::IVec3 ab = atlas_->config().atlas_bricks;
	const ve::IVec3 cell{slot % ab.x, (slot / ab.x) % ab.y, slot / (ab.x * ab.y)};

	// texture_get_data returns the whole volume; tests run a small atlas, so one read each.
	const PackedByteArray sdf = device->texture_get_data(atlas_->sdf_atlas(), 0);
	const PackedByteArray mat = device->texture_get_data(atlas_->mat_atlas(), 0);
	const int sw = ab.x * ve::kBrickSdfStride, sh = ab.y * ve::kBrickSdfStride;
	const int mw = ab.x * ve::kBrickVoxels, mh = ab.y * ve::kBrickVoxels;

	int sdf_max = 0, sdf_over_one = 0;
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const int ax = cell.x * ve::kBrickSdfStride + x;
				const int ay = cell.y * ve::kBrickSdfStride + y;
				const int az = cell.z * ve::kBrickSdfStride + z;
				const int got = sdf[ax + ay * sw + az * sw * sh];
				const int want = ref.brick.sdf[ve::sdf_index(x, y, z)];
				const int diff = std::abs(got - want);
				sdf_max = std::max(sdf_max, diff);
				if (diff > 1) sdf_over_one++;
			}
	d["sdf_max_diff"] = sdf_max;
	d["sdf_diff_over_one"] = sdf_over_one;

	const PackedByteArray pal_bytes = device->buffer_get_data(atlas_->palette(),
			static_cast<uint32_t>(slot) * ve::kBrickPaletteSize * 4,
			ve::kBrickPaletteSize * 4);
	const uint32_t *pal = reinterpret_cast<const uint32_t *>(pal_bytes.ptr());
	bool pal_ok = true;
	bool has_four = false;
	for (int p = 0; p < ve::kBrickPaletteSize; p++) {
		pal_ok = pal_ok && pal[p] == ref.brick.palette[p];
		has_four = has_four || pal[p] == 4;
	}
	d["palette_match"] = pal_ok;
	d["has_material_4"] = has_four;

	// Materials are only meaningful where a hit point can land — within ~1.2 voxels of the
	// surface. Compare RESOLVED ids, not packed indices: the two sides agree on the palette
	// ordering, but comparing ids keeps the check honest if that ever changes.
	int near_compared = 0, near_mismatch = 0;
	for (int z = 0; z < ve::kBrickVoxels; z++)
		for (int y = 0; y < ve::kBrickVoxels; y++)
			for (int x = 0; x < ve::kBrickVoxels; x++) {
				const float dist = ve::decode_sdf(ref.brick.sdf[ve::sdf_index(x, y, z)]);
				if (std::fabs(dist) > 1.2f * ve::kVoxelSize) continue;
				const int ax = cell.x * ve::kBrickVoxels + x;
				const int ay = cell.y * ve::kBrickVoxels + y;
				const int az = cell.z * ve::kBrickVoxels + z;
				const int gi = mat[ax + ay * mw + az * mw * mh];
				const uint32_t got_id = gi < ve::kBrickPaletteSize ? pal[gi] : 0;
				const uint16_t want_id =
						ref.brick.palette[ve::get_mat_index(ref.brick, ve::voxel_index(x, y, z))];
				near_compared++;
				if (got_id != want_id) near_mismatch++;
			}
	d["mat_near_compared"] = near_compared;
	d["mat_near_mismatch"] = near_mismatch;

	int mip_bad = 0;
	// The reference mips are reduced from the GPU lattice just read back, not from the CPU
	// one. The SDF diff already tolerates a one-step sin() drift (glibc vs driver), and a
	// drifted extremum would otherwise flag a mip cell that is a perfectly correct
	// reduction of what the GPU actually wrote (brief Step 5 note: "the property under
	// test is that the reduction is right, not that sin is bit-identical").
	uint8_t gpu_lattice[ve::kBrickSdfCount];
	for (int z = 0; z < ve::kBrickSdfStride; z++)
		for (int y = 0; y < ve::kBrickSdfStride; y++)
			for (int x = 0; x < ve::kBrickSdfStride; x++) {
				const int ax = cell.x * ve::kBrickSdfStride + x;
				const int ay = cell.y * ve::kBrickSdfStride + y;
				const int az = cell.z * ve::kBrickSdfStride + z;
				gpu_lattice[ve::sdf_index(x, y, z)] = sdf[ax + ay * sw + az * sw * sh];
			}
	ve::BrickMips ref_mips{};
	ve::build_brick_mips(gpu_lattice, &ref_mips);
	for (int level = 0; level < ve::kMipLevels; level++) {
		const int dim = ve::kMipDims[level];
		const PackedByteArray mip = device->texture_get_data(atlas_->mip_atlas(level), 0);
		const int w = ab.x * dim, h = ab.y * dim;
		const uint8_t *want_mn = ve::mip_min(ref_mips, level);
		const uint8_t *want_mx = ve::mip_max(ref_mips, level);
		for (int z = 0; z < dim; z++)
			for (int y = 0; y < dim; y++)
				for (int x = 0; x < dim; x++) {
					const int ax = cell.x * dim + x, ay = cell.y * dim + y, az = cell.z * dim + z;
					const int64_t o = (static_cast<int64_t>(ax) + ay * w + az * w * h) * 2;
					const int i = x + y * dim + z * dim * dim;
					if (mip[o] != want_mn[i] || mip[o + 1] != want_mx[i]) mip_bad++;
				}
	}
	d["mip_mismatch"] = mip_bad;
	return d;
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

RID VoxelWorld::debug_sdf_atlas() const { return atlas_ ? atlas_->sdf_atlas() : RID(); }
RID VoxelWorld::debug_mat_atlas() const { return atlas_ ? atlas_->mat_atlas() : RID(); }
RID VoxelWorld::debug_mip_atlas(int level) const { return atlas_ ? atlas_->mip_atlas(level) : RID(); }
RID VoxelWorld::debug_region_map() const { return atlas_ ? atlas_->region_map() : RID(); }
RID VoxelWorld::debug_region_tables() const { return atlas_ ? atlas_->region_tables() : RID(); }
RID VoxelWorld::debug_free_list() const { return atlas_ ? atlas_->free_list() : RID(); }
RID VoxelWorld::debug_frame_counters() const { return atlas_ ? atlas_->frame_counters() : RID(); }
RID VoxelWorld::debug_op_pool() const { return atlas_ ? atlas_->op_pool() : RID(); }
RID VoxelWorld::debug_op_counts() const { return atlas_ ? atlas_->op_counts() : RID(); }

int VoxelWorld::debug_stream_frame(Vector3 cam) {
	ensure_initialized();
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !streamer_) return 0;
	const int actions = streamer_->run_frame(device, cam.x, cam.y, cam.z);
	device->submit();
	device->sync();
	overflow_seen_ |= static_cast<int>(atlas_->read_overflow(device));
	return actions;
}

Dictionary VoxelWorld::debug_stream_stats() {
	Dictionary d;
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !residency_ || !streamer_) return d;
	d["resident_regions"] = residency_->resident_count();
	d["frame_edits"] = streamer_->last_frame_edits();
	d["overflow"] = static_cast<int>(atlas_->read_overflow(device));
	d["overflow_ever"] = overflow_seen_;
	return d;
}

int VoxelWorld::debug_slot_of_region(Vector3i region) const {
	if (!residency_) return -1;
	return residency_->slot_of({region.x, region.y, region.z});
}

int VoxelWorld::debug_region_map_entry(Vector3i region) {
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_) return -1;
	const int idx = world_bounds().region_index({region.x, region.y, region.z});
	if (idx < 0) return -1;
	const PackedByteArray b = device->buffer_get_data(atlas_->region_map(), idx * 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : -1;
}

bool VoxelWorld::debug_region_map_consistent() {
	RenderingDevice *device = rd();
	if (!initialized_ || !device || !atlas_ || !residency_) return false;
	const ve::WorldBounds wb = world_bounds();
	const PackedByteArray b = device->buffer_get_data(atlas_->region_map());
	const int32_t *map = reinterpret_cast<const int32_t *>(b.ptr());
	const ve::IVec3 o = wb.origin_regions();
	const ve::IVec3 sz = wb.size_regions;
	for (int z = 0; z < sz.z; z++)
		for (int y = 0; y < sz.y; y++)
			for (int x = 0; x < sz.x; x++) {
				const ve::IVec3 r{o.x + x, o.y + y, o.z + z};
				const int gpu_slot = map[x + y * sz.x + z * sz.x * sz.y];
				const int cpu_slot = residency_->slot_of(r);
				if (gpu_slot != cpu_slot) return false;
				if (gpu_slot >= 0 && !(residency_->region_of_slot(gpu_slot) == r)) return false;
			}
	return true;
}

Dictionary VoxelWorld::debug_raycast(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!edit_log_) return d;
	std::lock_guard<std::mutex> lock(edit_mutex_);
	ve::AnalyticGenerator gen;
	const float o[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::RayHit h = ve::raycast(gen, *edit_log_, o, f, 200.0f);
	if (!h.hit) return d;
	d["hit"] = true;
	d["pos"] = Vector3(h.pos[0], h.pos[1], h.pos[2]);
	d["normal"] = Vector3(h.normal[0], h.normal[1], h.normal[2]);
	d["distance"] = h.distance;
	return d;
}
