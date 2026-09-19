#include "render/leaf_scatter_pass.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include "render/gpu_atlas.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

LeafScatterPass::~LeafScatterPass() {
	teardown();
}

bool LeafScatterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	trees_ = gpu::compile_compute(rd, group_, "LeafScatterPass", "leaf_trees.comp.glsl");
	if (!trees_.valid()) {
		teardown();
		return false;
	}
	// Stage 2 is not optional any more: a pass that culls but cannot scatter would report
	// trees forever while drawing nothing, which is the failure mode the design doc calls
	// worse than no leaves at all. The orchestrator drops the whole pass on a false return,
	// so this still cannot abort a frame.
	scatter_ = gpu::compile_compute(rd, group_, "LeafScatterPass", "leaf_scatter.comp.glsl");
	if (!scatter_.valid()) {
		teardown();
		return false;
	}
	// Owned sampler pair for the atlas textures. Mirrors RaymarchPass: the SDF atlas
	// filters linearly, the integer material atlas must stay nearest.
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	if (!sampler_linear_.is_valid() || !sampler_nearest_.is_valid()) {
		teardown();
		return false;
	}
	// Allocate the default-sized buffers eagerly. Tests (and debug_leaf_stats) never drive
	// the compositor, so a capacity that only appears after the first frame would read as
	// zero there; run() still re-sizes to the live layout before every dispatch.
	// leaf_layout() leaves params.wind[3] at zero by contract -- time is run()'s to write.
	const ve::LeafSettings defaults;
	const float origin[3] = {0.0f, 0.0f, 0.0f};
	static const float kIdentity[16] = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
		0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
	const ve::LeafLayout warm = ve::leaf_layout(defaults, origin, kIdentity);
	if (!ensure_buffers(rd, warm.params.limits[0], warm.params.limits[1])) {
		teardown();
		return false;
	}
	return true;
}

void LeafScatterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	trees_ = scatter_ = gpu::Program();
	params_ubo_ = tree_list_ = counters_ = dispatch_args_ = draw_args_ = instances_ = RID();
	region_ubo_ = field_ops_ = sampler_linear_ = sampler_nearest_ = RID();
	trees_set_ = scatter_set_ = gpu::SetCache();
	capacity_ = 0;
	tree_capacity_ = 0;
	last_tree_count_ = 0;
	last_clump_count_ = 0;
	clump_high_water_ = 0;
	sample_count_ = 0;
	sample_max_crown_offset_ = 0.0f;
	overflow_logged_ = false;
	rd_ = nullptr;
}

bool LeafScatterPass::ensure_buffers(RenderingDevice *rd, int max_clumps, int max_trees) {
	if (max_clumps <= 0 || max_trees <= 0) return false;
	if (instances_.is_valid() && capacity_ == max_clumps && tree_capacity_ >= max_trees)
		return true;
	// Freeing a buffer takes the uniform sets that bind it; the cache rebuilds on new RIDs.
	gpu::RdDevice device{rd};
	for (RID *r : {&instances_, &tree_list_, &counters_, &dispatch_args_, &draw_args_,
			&params_ubo_, &region_ubo_, &field_ops_}) {
		group_.free(device, *r);
		*r = RID();
	}
	// 32 bytes per clump: two vec4, the same budget as a GrassBlade. Stage 2 writes this
	// buffer from the first frame it runs; the contract says the pass owns its size, so it
	// is allocated from settings.max_clumps from the first frame.
	instances_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(max_clumps) * 32u));
	// 32 bytes per tree: LeafTree in shaders/leaf.glslh, two vec4.
	tree_list_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(max_trees) * 32u));
	counters_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u));
	dispatch_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(12u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	// Non-indexed indirect draw args for Task 12's raster: vertex_count, instance_count,
	// first_vertex, first_instance -- the same shape as GrassScatterPass::draw_args_.
	draw_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u,
			PackedByteArray(), RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	params_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::LeafParams)));
	// Region-window block for binding 9 (three ivec4: dims, region_origin, atlas_bricks).
	// Contents refresh every run(); the RID is stable so the uniform set survives across
	// frames -- cached against the RID, never rebuilt per frame.
	region_ubo_ = group_.add(gpu::Kind::Buffer,
			rd->uniform_buffer_create(sizeof(ve::GrassRegionBlock)));
	// One EditOp (32 bytes) of zeroes. field.glslh declares the op pool; the leaf scatter
	// evaluates the ANALYTIC ground only, so it is never read -- but a declared binding
	// still has to be provided, the same rule the unsampled atlas textures follow above.
	field_ops_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(32u));
	capacity_ = max_clumps;
	tree_capacity_ = max_trees;
	return instances_.is_valid() && tree_list_.is_valid() && counters_.is_valid() &&
			dispatch_args_.is_valid() && draw_args_.is_valid() && params_ubo_.is_valid() &&
			region_ubo_.is_valid() && field_ops_.is_valid();
}

bool LeafScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo) {
	gpu::RdDevice device{rd};
	// Stage-1 set: 0 leaf-params, 1 tree_list, 2 counters, 3 dispatch_args, 4 region_map,
	// 5 region_tables, 6 region_slot_counts, 7 sdf_atlas, 8 mat_atlas, 9 region UBO,
	// 10 palette_buf, 11 brick_flags (both needed by brick_atlas.glslh's helpers), 12 the
	// field op pool field.glslh declares. The terrain pipeline's set 1 is bound separately
	// in run(), from the caller-supplied FieldContextSet. Texture/sampler RIDs come from
	// GpuAtlas through the same accessors RaymarchPass uses.
	const RID trees = trees_set_.get(device, group_, trees_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, tree_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, dispatch_args_),
			gpu::storage(4, atlas.region_map()),
			gpu::storage(5, atlas.region_tables()),
			gpu::storage(6, atlas.region_slot_counts()),
			gpu::sampled(7, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(8, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(9, region_ubo_),
			gpu::storage(10, atlas.palette()),
			gpu::storage(11, atlas.brick_flags()),
			gpu::storage(12, field_ops_)});
	if (!trees.is_valid()) return false;
	// Stage-2 set (leaf_scatter.comp.glsl bindings 0-13): 0 leaf-params, 1 tree_list,
	// 2 counters, 3 the raster draw_args (NOT dispatch_args -- the dispatch itself is
	// indirect through the driver, like grass's), 4 instances, 5 the frame's SunUbo,
	// 6-8 the region buffers, 9-10 the atlas textures, 11 region UBO, and 12-13 the
	// palette/brick_flags pair brick_atlas.glslh forces, mirroring stage 1's 10-11. The
	// brief's snippet numbers stop at 11; these two are the same forced appendix the
	// leaf_trees landing documented.
	// An absent SunUbo or slot-count buffer skips stage 2's set for this frame (the cull
	// still runs -- frame.cpp's contract), it does not fail the pass.
	if (!sun_ubo.is_valid() || !atlas.region_slot_counts().is_valid()) return true;
	return scatter_set_.get(device, group_, scatter_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, tree_list_),
			gpu::storage(2, counters_),
			gpu::storage(3, draw_args_),
			gpu::storage(4, instances_),
			gpu::ubo(5, sun_ubo),
			gpu::storage(6, atlas.region_map()),
			gpu::storage(7, atlas.region_tables()),
			gpu::storage(8, atlas.region_slot_counts()),
			gpu::sampled(9, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(10, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(11, region_ubo_),
			gpu::storage(12, atlas.palette()),
			gpu::storage(13, atlas.brick_flags())}).is_valid();
}

bool LeafScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas,
		const ve::LeafLayout &layout, const ve::RegionWindow &region_win,
		float time_seconds, RID sun_ubo, const FieldContextSet *field) {
	last_tree_count_ = 0;
	last_clump_count_ = 0;
	if (!rd_ || rd != rd_ || !trees_.valid() || !scatter_.valid()) return false;
	// Stage 2 needs the sun (its march reads the SunUbo) and the residency slot counts
	// (the march early-outs outside the resident field). Either missing drops stage 2 for
	// the frame, not the cull: canopies are decorative, trunks keep drawing.
	const bool scatter_ok = sun_ubo.is_valid() && atlas.region_slot_counts().is_valid();
	// The trees stage's ground functions live in the generated field source, so set 1 is
	// not optional here the way it is for grass's near field: without a field context the
	// dispatch would read undefined params. Fail the pass (the caller cancels the timing
	// marker), never the frame.
	if (!field || !field->is_valid()) return false;
	if (layout.dispatch_threads <= 0 || layout.params.limits[0] <= 0 ||
			layout.params.limits[1] <= 0) {
		// Disabled: clear the GPU counters AND both indirect arg buffers, not just the CPU
		// mirrors zeroed above. debug_leaf_stats() re-reads the GPU counters after its own
		// submit+sync, so stale counters (clump_count, high_water and the pad reduction
		// included) would report the previous frame's counts; stale dispatch args would
		// scatter the previous frame's trees and stale draw args would be worse still --
		// Task 12's raster would re-draw frozen clumps while the CPU reports zero. A cleared
		// dispatch is a no-op.
		// A successful no-op, not a failure. The caller still ends its timing marker.
		PackedByteArray zero_gpu;
		zero_gpu.resize(16);
		zero_gpu.fill(0);
		if (counters_.is_valid()) rd->buffer_update(counters_, 0, 16, zero_gpu);
		if (draw_args_.is_valid()) rd->buffer_update(draw_args_, 0, 16, zero_gpu);
		if (dispatch_args_.is_valid()) {
			PackedByteArray seed;
			seed.resize(12);
			uint32_t *w = reinterpret_cast<uint32_t *>(seed.ptrw());
			w[0] = 0u; w[1] = 1u; w[2] = 1u;
			rd->buffer_update(dispatch_args_, 0, 12, seed);
		}
		return true;
	}
	if (!ensure_buffers(rd, layout.params.limits[0], layout.params.limits[1])) return false;
	if (!ensure_uniform_sets(rd, atlas, sun_ubo)) return false;

	ve::LeafParams params = layout.params;
	// R4: params.wind[3] is TIME, written here and only here; leaf_layout() leaves it 0.
	params.wind[3] = time_seconds;
	rd->buffer_update(params_ubo_, 0, sizeof(params), gpu::push_bytes(params));

	// Refresh the pass-owned region window from the LIVE residency-backed window the caller
	// threads through (WorldStore::region_window(), same source debug_grass_stats uses):
	// atlas.config().region_window is init-centred/stale, so trees would vanish away from
	// the origin. The UBO RID is stable, so the cached uniform set survives; only contents
	// change.
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
	// Same for the indirect dispatch args: stage 1 grows x with atomicMax, which assumes a
	// zero base; y and z are 1 so the buffer is a legal 3D dispatch for stage 2 even before
	// stage 1 writes it. Unlike grass, no thread seeds these in-shader: C++ owns the seed.
	PackedByteArray seed;
	seed.resize(12);
	{
		uint32_t *w = reinterpret_cast<uint32_t *>(seed.ptrw());
		w[0] = 0u; w[1] = 1u; w[2] = 1u;
	}
	rd->buffer_update(dispatch_args_, 0, 12, seed);
	// Same for the raster draw args: stage 2 grows vertex_count with atomicMax, which
	// assumes a zero base, exactly like grass's draw_args_.
	PackedByteArray zero_args;
	zero_args.resize(16);
	zero_args.fill(0);
	rd->buffer_update(draw_args_, 0, 16, zero_args);

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, trees_.pipeline);
	rd->compute_list_bind_uniform_set(list, trees_set_.id(), 0);
	field->bind(rd, list);
	rd->compute_list_dispatch(list, (layout.dispatch_threads + 63) / 64, 1, 1);
	if (scatter_ok) {
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, scatter_.pipeline);
		rd->compute_list_bind_uniform_set(list, scatter_set_.id(), 0);
		rd->compute_list_dispatch_indirect(list, dispatch_args_, 0);
	}
	rd->compute_list_end();
	read_back_counters(rd);
	return true;
}

void LeafScatterPass::read_back_counters(RenderingDevice *rd) {
	const PackedByteArray data = rd->buffer_get_data(counters_, 0, 16);
	if (data.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	// Overflow is clamped, never scribbled: each stage atomicMin's its own count back to
	// capacity, so the counts themselves can never show the drop -- counters.high_water (the
	// slot+1 every stage-2 thread atomicMaxes BEFORE the clamp test) is what records how
	// many clumps the frame actually wanted. Clamp again CPU-side so a torn read can never
	// exceed the buffer.
	last_tree_count_ = static_cast<int>(std::min<uint32_t>(c[0], static_cast<uint32_t>(tree_capacity_)));
	last_clump_count_ = static_cast<int>(std::min<uint32_t>(c[1], static_cast<uint32_t>(capacity_)));
	clump_high_water_ = std::max(clump_high_water_, static_cast<int>(c[2]));
	const bool overflowed = static_cast<int>(c[2]) > capacity_ ||
			static_cast<int>(c[0]) > tree_capacity_;
	if (overflowed && !overflow_logged_) {
		overflow_logged_ = true;
		UtilityFunctions::printerr("LeafScatterPass: buffer overflow, wanted ",
				static_cast<int>(c[2]), " clumps of ", capacity_, " / ",
				static_cast<int>(c[0]), " trees of ", tree_capacity_,
				"; entries were dropped. Lower clumps_per_tree or raise max_clumps/max_trees.");
	}
}

void LeafScatterPass::read_back_sample(RenderingDevice *rd) {
	sample_count_ = std::min(last_clump_count_, 4096);
	// The CPU-reduce alternative grass's read_back_sample does needs the crown centre, which
	// is not in the instance record. Stage 2 therefore reduces the containment metric on the
	// GPU -- counters.pad holds the widest fixed-point (shell reach / crown radius) any
	// emitted clump achieved -- and this reads that word. At most one 16-byte buffer read,
	// so "at most the first 4096 instances" is instead "exactly what the shader saw".
	sample_max_crown_offset_ = 0.0f;
	if (sample_count_ <= 0) return;
	const PackedByteArray data = rd->buffer_get_data(counters_, 0, 16);
	if (data.size() < 16) { sample_count_ = 0; return; }
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	sample_max_crown_offset_ = static_cast<float>(c[3]) / 65536.0f;
}
