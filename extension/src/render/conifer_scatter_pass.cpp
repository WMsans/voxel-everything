#include "render/conifer_scatter_pass.h"
#include "render/field_context_set.h"
#include "render/gpu_atlas.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>

using namespace godot;

ConiferScatterPass::~ConiferScatterPass() { teardown(); }

bool ConiferScatterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;
	cull_ = gpu::compile_compute(rd, group_, "ConiferScatterPass", "conifer_cull.comp.glsl");
	scatter_ = gpu::compile_compute(rd, group_, "ConiferScatterPass", "conifer_scatter.comp.glsl");
	sampler_nearest_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	hiz_dummy_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32_SFLOAT, Vector2i(1, 1),
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	counters_read_.instantiate();
	if (!cull_.valid() || !hiz_dummy_.is_valid() || !scatter_.valid() || !sampler_linear_.is_valid() ||
			!sampler_nearest_.is_valid() || counters_read_.is_null()) {
		teardown();
		return false;
	}
	// Default-sized buffers up front, as LeafScatterPass does, so hooks see a capacity.
	ve::ConiferSettings defaults;
	ve::ConiferPassParams p{};
	p.limits[0] = defaults.max_clumps;
	p.limits[1] = defaults.max_card_trees;
	p.limits[3] = defaults.max_impostors;
	if (!ensure_buffers(rd, p)) {
		teardown();
		return false;
	}
	return true;
}

void ConiferScatterPass::teardown() {
	if (!rd_) return;
	if (counters_read_.is_valid()) counters_read_->drain(rd_);
	counters_read_ = Ref<AsyncBufferRead>();
	gpu::RdDevice device{rd_};
	group_.release(device);
	cull_ = scatter_ = gpu::Program();
	params_ubo_ = raster_ubo_ = region_ubo_ = field_ops_ = RID();
	card_list_ = impostor_list_ = counters_ = dispatch_args_ = draw_args_ = impostor_args_ = instances_ = RID();
	sampler_linear_ = sampler_nearest_ = hiz_dummy_ = RID();
	cull_set_ = scatter_set_ = gpu::SetCache();
	max_clumps_ = max_card_trees_ = max_impostors_ = 0;
	last_card_trees_ = last_impostors_ = last_clumps_ = clump_high_water_ = 0;
	overflow_logged_ = false;
	rd_ = nullptr;
}

bool ConiferScatterPass::ensure_buffers(RenderingDevice *rd, const ve::ConiferPassParams &p) {
	const int clumps = p.limits[0], cards = p.limits[1], imps = p.limits[3];
	if (clumps <= 0 || cards <= 0 || imps <= 0) return false;
	if (instances_.is_valid() && clumps == max_clumps_ && cards == max_card_trees_ && imps == max_impostors_)
		return true;
	if (counters_read_.is_valid()) counters_read_->drain(rd);
	gpu::RdDevice device{rd};
	for (RID *r : {&instances_, &card_list_, &impostor_list_, &counters_, &dispatch_args_, &draw_args_,
			&impostor_args_, &params_ubo_, &raster_ubo_, &region_ubo_, &field_ops_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const auto indirect = RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT;
	instances_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(uint32_t(clumps) * 32u));
	card_list_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(uint32_t(cards) * 32u));
	impostor_list_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(uint32_t(imps) * 32u));
	counters_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u));
	dispatch_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(12u, PackedByteArray(), indirect));
	draw_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u, PackedByteArray(), indirect));
	impostor_args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(16u, PackedByteArray(), indirect));
	params_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::ConiferPassParams)));
	raster_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::LeafParams)));
	region_ubo_ = group_.add(gpu::Kind::Buffer, rd->uniform_buffer_create(sizeof(ve::GrassRegionBlock)));
	// One EditOp of zeroes: field.glslh declares the op pool; the cull never indexes it.
	field_ops_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(32u));
	max_clumps_ = clumps;
	max_card_trees_ = cards;
	max_impostors_ = imps;
	return instances_.is_valid() && card_list_.is_valid() && impostor_list_.is_valid() &&
			counters_.is_valid() && dispatch_args_.is_valid() && draw_args_.is_valid() &&
			impostor_args_.is_valid() && params_ubo_.is_valid() && raster_ubo_.is_valid() &&
			region_ubo_.is_valid() && field_ops_.is_valid();
}

bool ConiferScatterPass::ensure_uniform_sets(RenderingDevice *rd, GpuAtlas &atlas, RID sun_ubo, RID hiz) {
	gpu::RdDevice device{rd};
	const RID cull = cull_set_.get(device, group_, cull_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, card_list_),
			gpu::storage(2, impostor_list_),
			gpu::storage(3, counters_),
			gpu::storage(4, dispatch_args_),
			gpu::storage(5, impostor_args_),
			gpu::storage(6, atlas.region_map()),
			gpu::storage(7, atlas.region_tables()),
			gpu::storage(8, atlas.region_slot_counts()),
			gpu::sampled(9, sampler_linear_, atlas.sdf_atlas()),
			gpu::sampled(10, sampler_nearest_, atlas.mat_atlas()),
			gpu::ubo(11, region_ubo_),
			gpu::storage(12, atlas.palette()),
			gpu::storage(13, atlas.brick_flags()),
			gpu::storage(14, field_ops_),
			gpu::sampled(15, sampler_nearest_, hiz.is_valid() ? hiz : hiz_dummy_)});
	if (!cull.is_valid()) return false;
	if (!sun_ubo.is_valid()) return true; // no sun: the cull runs, the scatter is skipped
	return scatter_set_.get(device, group_, scatter_.shader, 0, {
			gpu::ubo(0, params_ubo_),
			gpu::storage(1, card_list_),
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

void ConiferScatterPass::clear_args(RenderingDevice *rd) {
	PackedByteArray zero;
	zero.resize(16);
	zero.fill(0);
	rd->buffer_update(counters_, 0, 16, zero);
	rd->buffer_update(draw_args_, 0, 16, zero);
	rd->buffer_update(impostor_args_, 0, 16, zero);
	PackedByteArray seed;
	seed.resize(12);
	uint32_t *w = reinterpret_cast<uint32_t *>(seed.ptrw());
	w[0] = 0u; w[1] = 1u; w[2] = 1u;
	rd->buffer_update(dispatch_args_, 0, 12, seed);
}

bool ConiferScatterPass::run(RenderingDevice *rd, GpuAtlas &atlas, const ve::ConiferLayout &layout,
		const ve::RegionWindow &region_win, float time_seconds, RID sun_ubo,
		const FieldContextSet *field, const float view_proj[16], RID hiz, int hiz_size,
		int hiz_mips) {
	if (!rd_ || rd != rd_ || !cull_.valid() || !scatter_.valid()) return false;
	if (!field || !field->is_valid()) return false;
	if (layout.dispatch_threads <= 0) {
		// Disabled: clear every count and arg buffer so neither raster redraws frozen trees,
		// and drop any read still in flight. A successful no-op.
		if (counters_read_.is_valid()) counters_read_->take_fresh();
		last_card_trees_ = last_impostors_ = last_clumps_ = 0;
		clear_args(rd);
		return true;
	}
	if (!ensure_buffers(rd, layout.params)) return false;
	if (!ensure_uniform_sets(rd, atlas, sun_ubo, hiz)) return false;
	const bool scatter_ok = sun_ubo.is_valid() && atlas.region_slot_counts().is_valid();

	rd->buffer_update(params_ubo_, 0, sizeof(layout.params), gpu::push_bytes(layout.params));
	ve::LeafParams raster = layout.raster;
	raster.wind[3] = time_seconds; // time is run()'s to write, as LeafScatterPass's
	rd->buffer_update(raster_ubo_, 0, sizeof(raster), gpu::push_bytes(raster));
	const ve::IVec3 ab = atlas.config().atlas_bricks;
	const ve::GrassRegionBlock region{{region_win.dim, region_win.dim, region_win.dim, 0},
			{region_win.origin.x, region_win.origin.y, region_win.origin.z, 0},
			{ab.x, ab.y, ab.z, 0}};
	rd->buffer_update(region_ubo_, 0, sizeof(region), gpu::push_bytes(region));
	clear_args(rd);

	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, cull_.pipeline);
	rd->compute_list_bind_uniform_set(list, cull_set_.id(), 0);
	field->bind(rd, list);
	ve::LodCullPush push{};
	std::copy(view_proj, view_proj + 16, push.view_proj);
	push.params[0] = hiz.is_valid() ? 1 : 0;
	push.params[1] = hiz_size;
	push.params[2] = hiz_mips;
	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, (layout.dispatch_threads + 63) / 64, 1, 1);
	if (scatter_ok) {
		rd->compute_list_add_barrier(list);
		rd->compute_list_bind_compute_pipeline(list, scatter_.pipeline);
		rd->compute_list_bind_uniform_set(list, scatter_set_.id(), 0);
		rd->compute_list_dispatch_indirect(list, dispatch_args_, 0);
	}
	rd->compute_list_end();
	if (counters_read_.is_valid()) {
		if (counters_read_->take_fresh()) apply_counters(counters_read_->data());
		counters_read_->request(rd, counters_, 0, 16);
	}
	return true;
}

void ConiferScatterPass::read_back_counters(RenderingDevice *rd) {
	if (counters_read_.is_valid()) {
		counters_read_->drain(rd);
		counters_read_->take_fresh();
	}
	apply_counters(rd->buffer_get_data(counters_, 0, 16));
}

void ConiferScatterPass::apply_counters(const PackedByteArray &data) {
	if (data.size() < 16) return;
	const uint32_t *c = reinterpret_cast<const uint32_t *>(data.ptr());
	last_card_trees_ = int(std::min<uint32_t>(c[0], uint32_t(max_card_trees_)));
	last_impostors_ = int(std::min<uint32_t>(c[1], uint32_t(max_impostors_)));
	last_clumps_ = int(std::min<uint32_t>(c[2], uint32_t(max_clumps_)));
	clump_high_water_ = std::max(clump_high_water_, int(c[3]));
	if (int(c[3]) > max_clumps_ && !overflow_logged_) {
		overflow_logged_ = true;
		UtilityFunctions::printerr("ConiferScatterPass: clump buffer overflow, wanted ", int(c[3]),
				" of ", max_clumps_, "; raise max_clumps or lower clumps_per_tree.");
	}
}
