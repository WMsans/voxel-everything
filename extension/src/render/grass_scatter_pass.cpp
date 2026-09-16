#include "render/grass_scatter_pass.h"
#include "gpu_layout/blocks.h"
#include "render/gpu_atlas.h"
#include "shade/oct.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

GrassScatterPass::~GrassScatterPass() {
	teardown();
}

bool GrassScatterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	bricks_ = gpu::compile_compute(rd, group_, "GrassScatterPass", "grass_bricks.comp.glsl");
	if (!bricks_.valid()) {
		teardown();
		return false;
	}
	// grass_scatter.comp.glsl arrives in Task 6. Until then the scatter stage stays
	// invalid and run() skips stage 2 -- the pass still culls bricks and reports zeros.
	scatter_ = gpu::compile_compute(rd, group_, "GrassScatterPass", "grass_scatter.comp.glsl");
	if (!scatter_.valid()) scatter_ = gpu::Program();
	// Owned sampler pair for the atlas textures stage 1 declares but never samples.
	// Mirrors RaymarchPass: the SDF atlas filters linearly, the integer material atlas
	// must stay nearest.
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	if (!sampler_linear_.is_valid() || !sampler_nearest_.is_valid()) {
		teardown();
		return false;
	}
	// Allocate the default-sized buffers eagerly. Tests (and debug_grass_stats) never drive
	// the compositor, so a capacity that only appears after the first frame would read as
	// zero there; run() still re-sizes to the live layout before every dispatch.
	const ve::GrassSettings defaults;
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	static const float kIdentity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	const ve::GrassLayout warm = ve::grass_layout(defaults, origin, kIdentity);
	if (!ensure_buffers(rd, warm.params.limits[0], warm.max_bricks)) {
		teardown();
		return false;
	}
	return true;
}

void GrassScatterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	bricks_ = scatter_ = gpu::Program();
	params_ubo_ = brick_list_ = counters_ = dispatch_args_ = instances_ = draw_args_ = RID();
	region_ubo_ = sampler_linear_ = sampler_nearest_ = RID();
	bricks_set_ = scatter_set_ = gpu::SetCache();
	sample_count_ = 0;
	sample_min_normal_y_ = 1.0f;
	sample_max_height_ = 0.0f;
	sample_min_sun_ = 1.0f;
	sample_max_sun_ = 0.0f;
	sample_mean_sun_ = 0.0f;
	capacity_ = 0;
	brick_capacity_ = 0;
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	overflow_logged_ = false;
	rd_ = nullptr;
}

bool GrassScatterPass::ensure_buffers(RenderingDevice *rd, int max_blades, int max_bricks) {
	if (max_blades <= 0 || max_bricks <= 0) return false;
	if (instances_.is_valid() && capacity_ == max_blades && brick_capacity_ >= max_bricks)
		return true;
	// Freeing a buffer takes the uniform sets that bind it; both caches rebuild on new RIDs.
	gpu::RdDevice device{rd};
	for (RID *r : {&instances_, &brick_list_, &counters_, &dispatch_args_, &draw_args_,
			&params_ubo_, &region_ubo_}) {
		group_.free(device, *r);
		*r = RID();
	}
	// 32 bytes per blade: two vec4 (design doc section 5).
	instances_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(max_blades) * 32u));
	brick_list_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(max_bricks) * 4u));
	counters_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u));
	dispatch_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(12u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	// Non-indexed indirect draw args: vertexCount, instanceCount, firstVertex, firstInstance.
	draw_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	params_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::GrassParams)));
	// Region-window block for stage 1's binding 10 (three ivec4: dims, region_origin,
	// atlas_bricks). Contents refresh every run(); the RID is stable so the uniform set
	// survives across frames -- cached against the RID, never rebuilt per frame.
	region_ubo_ = group_.add(gpu::Kind::Buffer,
			rd->uniform_buffer_create(sizeof(ve::GrassRegionBlock)));
	capacity_ = max_blades;
	brick_capacity_ = max_bricks;
	return instances_.is_valid() && brick_list_.is_valid() && counters_.is_valid() &&
			dispatch_args_.is_valid() && draw_args_.is_valid() && params_ubo_.is_valid() &&
			region_ubo_.is_valid();
}

bool GrassScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo) {
	gpu::RdDevice device{rd};
	// Stage-1 set: 0 grass-params, 1 brick_list, 2 counters, 3 dispatch_args, 4 region_map,
	// 5 region_tables, 6 brick_flags, 7 sdf_atlas, 8 mat_atlas, 9 palette_buf, 10 region UBO.
	// Texture/sampler RIDs come from GpuAtlas through the same accessors RaymarchPass uses.
	// Bindings 7-8 are the atlas textures the sampling helpers declare but stage 1 never
	// fetches; a declared binding still has to be provided.
	const RID bricks = bricks_set_.get(device, group_, bricks_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, brick_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, dispatch_args_),
			gpu::storage(4, atlas.region_map()),
			gpu::storage(5, atlas.region_tables()),
			gpu::storage(6, atlas.brick_flags()),
			gpu::sampled(7, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(8, sampler_nearest_, atlas.mat_atlas()),
			gpu::storage(9, atlas.palette()),
			gpu::ubo(10, region_ubo_)});
	if (!bricks.is_valid()) return false;
	// Stage-2 set mirrors grass_scatter.comp.glsl bindings 0-13: 0 grass-params, 1
	// brick_list, 2 counters, 3 draw_args, 4 region_map, 5 region_tables, 6 brick_flags,
	// 7 palette_buf, 8 sdf_atlas, 9 mat_atlas, 10 region UBO, 11 instances, 12
	// region_slot_counts, 13 the frame's SunUbo -- the last two feed the sun march. Binding 3 is
	// draw_args_ here, NOT dispatch_args_ (that is stage 1's binding 3).
	if (!scatter_.valid()) return true; // scatter stage absent: cull only.
	if (!sun_ubo.is_valid() || !atlas.region_slot_counts().is_valid()) return false;
	return scatter_set_.get(device, group_, scatter_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, brick_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, draw_args_),
			gpu::storage(4, atlas.region_map()),
			gpu::storage(5, atlas.region_tables()),
			gpu::storage(6, atlas.brick_flags()),
			gpu::storage(7, atlas.palette()),
			gpu::sampled(8, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(9, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(10, region_ubo_),
			gpu::storage(11, instances_),
			gpu::storage(12, atlas.region_slot_counts()),
			gpu::ubo(13, sun_ubo)}).is_valid();
}

bool GrassScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas,
		const ve::GrassLayout &layout, const ve::RegionWindow &region_win,
		float time_seconds, RID sun_ubo) {
	last_brick_count_ = 0;
	last_blade_count_ = 0;
	if (!rd_ || rd != rd_ || !bricks_.valid()) return false;
	if (layout.max_bricks <= 0 || layout.params.limits[0] <= 0) {
		// Disabled: clear the GPU counters AND the indirect draw args, not just the CPU
		// mirrors zeroed above. debug_grass_stats() re-reads the GPU counters after its own
		// submit+sync, so stale counters would report the previous frame's counts;
		// stale draw args are worse -- the raster would re-draw the previous frame's
		// frozen blades while the CPU reports zero. An indirect draw with vertex_count
		// 0 is a no-op, so the stale params-UBO time needs no clearing.
		// A successful no-op, not a failure. The caller still ends its timing marker.
		PackedByteArray zero_gpu;
		zero_gpu.resize(16);
		zero_gpu.fill(0);
		if (counters_.is_valid()) rd->buffer_update(counters_, 0, 16, zero_gpu);
		if (draw_args_.is_valid()) rd->buffer_update(draw_args_, 0, 16, zero_gpu);
		return true;
	}
	if (!ensure_buffers(rd, layout.params.limits[0], layout.max_bricks)) return false;
	if (!ensure_uniform_sets(rd, atlas, sun_ubo)) return false;

	ve::GrassParams params = layout.params;
	params.wind[3] = time_seconds;
	rd->buffer_update(params_ubo_, 0, sizeof(params), gpu::push_bytes(params));

	// Refresh the pass-owned region window from the LIVE residency-backed window the
	// caller threads through (WorldStore::region_window(), same source debug_ssao_probe
	// uses): atlas.config().region_window is init-centred/stale, so grass would vanish
	// away from the origin. The UBO RID is stable, so the cached uniform set survives;
	// only contents change.
	{
		const ve::RegionWindow &win = region_win;
		const ve::IVec3 ab = atlas.config().atlas_bricks;
		const ve::GrassRegionBlock region{{win.dim, win.dim, win.dim, 0},
				{win.origin.x, win.origin.y, win.origin.z, 0}, {ab.x, ab.y, ab.z, 0}};
		rd->buffer_update(region_ubo_, 0, sizeof(region), gpu::push_bytes(region));
	}

	// Clear the counters explicitly. A fresh RD buffer reads back as zero on this machine,
	// so "the count was zero" must mean the pass wrote zero -- never that nobody wrote.
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	rd->buffer_update(counters_, 0, 16, zero);
	// Same for the indirect dispatch args: stage 1 grows dispatch_args.x with atomicMax,
	// which assumes a zero base. Thread 0 seeds y/z in-shader; x is seeded here because a
	// store would race the atomics.
	PackedByteArray zero_dispatch;
	zero_dispatch.resize(12);
	zero_dispatch.fill(0);
	rd->buffer_update(dispatch_args_, 0, 12, zero_dispatch);
	// Same for the indirect draw args: stage 2 grows vertex_count with atomicMax, which
	// assumes a zero base, and the raster would otherwise draw last frame's blades.
	PackedByteArray zero_args;
	zero_args.resize(16);
	zero_args.fill(0);
	rd->buffer_update(draw_args_, 0, 16, zero_args);

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, bricks_.pipeline);
	rd->compute_list_bind_uniform_set(list, bricks_set_.id(), 0);
	rd->compute_list_dispatch(list, (layout.max_bricks + 63) / 64, 1, 1);
	if (scatter_.valid()) {
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, scatter_.pipeline);
		rd->compute_list_bind_uniform_set(list, scatter_set_.id(), 0);
		rd->compute_list_dispatch_indirect(list, dispatch_args_, 0);
	}
	rd->compute_list_end();
	read_back_counters(rd);
	return true;
}

void GrassScatterPass::read_back_counters(RenderingDevice *rd) {
	const PackedByteArray data = rd->buffer_get_data(counters_, 0, 16);
	if (data.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	last_brick_count_ = static_cast<int>(c[0]);
	last_blade_count_ = static_cast<int>(std::min<uint32_t>(c[1], static_cast<uint32_t>(capacity_)));
	blade_high_water_ = std::max(blade_high_water_, static_cast<int>(c[1]));
	if (static_cast<int>(c[1]) > capacity_ && !overflow_logged_) {
		overflow_logged_ = true;
		UtilityFunctions::printerr("GrassScatterPass: blade buffer overflow, wanted ",
				static_cast<int>(c[1]), " of ", capacity_,
				"; blades were dropped. Lower blades_per_brick or raise max_blades.");
	}
}

void GrassScatterPass::read_back_sample(RenderingDevice *rd) {
	sample_min_normal_y_ = 1.0f;
	sample_max_height_ = 0.0f;
	sample_min_sun_ = 1.0f;
	sample_max_sun_ = 0.0f;
	sample_mean_sun_ = 0.0f;
	sample_count_ = std::min(last_blade_count_, 4096);
	if (sample_count_ <= 0) return;
	const PackedByteArray data = rd->buffer_get_data(instances_, 0,
			static_cast<uint32_t>(sample_count_) * 32u);
	if (data.size() < sample_count_ * 32) { sample_count_ = 0; return; }
	const float *f = reinterpret_cast<const float *>(data.ptr());
	for (int i = 0; i < sample_count_; i++) {
		const float *a = f + i * 8;      // xyz position, w height
		// b.x is grass_pack_ground(): the oct normal in the low 16 bits, the terrain sun
		// visibility byte in bits 16-23 (shaders/grass_blade.glslh).
		const uint32_t packed = static_cast<uint32_t>(a[4]);
		// oct_decode_snorm8's inverse for the y component only: the pass stores the ground
		// normal, and all this assertion needs is which way it points.
		const float ny = ve::oct_decode_y_snorm8(static_cast<uint16_t>(packed & 0xFFFFu));
		sample_min_normal_y_ = std::min(sample_min_normal_y_, ny);
		sample_max_height_ = std::max(sample_max_height_, a[3]);
		const float sun = static_cast<float>((packed >> 16) & 0xFFu) / 255.0f;
		sample_min_sun_ = std::min(sample_min_sun_, sun);
		sample_max_sun_ = std::max(sample_max_sun_, sun);
		sample_mean_sun_ += sun / static_cast<float>(sample_count_);
	}
}
