#include "render/raymarch_pass.h"
#include "render/field_context_set.h"
#include "render/gpu_atlas.h"
#include "render/island_atlas.h"
#include "render/material_atlas.h"
#include "gpu_layout/blocks.h"

using namespace godot;

RaymarchPass::~RaymarchPass() {
	teardown();
}

void RaymarchPass::initialize(RenderingDevice *rd) {
	rd_ = rd;
	program_ = gpu::compile_compute(rd, group_, "RaymarchPass", "raymarch.comp.glsl");
	if (!program_.valid()) return;
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	// Explicit clamp: brick_sdf() only ever asks for coordinates inside the brick's own
	// 17-voxel block, but an edge brick must not wrap to the far side of the atlas if a
	// coordinate lands exactly on the boundary.
	sampler_linear_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_LINEAR, true);
	PackedByteArray zero;
	zero.resize(sizeof(ve::EditsBlock));
	zero.fill(0);
	edits_ubo_ = group_.add(gpu::Kind::Buffer,
		rd->uniform_buffer_create(sizeof(ve::EditsBlock), zero));
}

void RaymarchPass::set_materials(const MaterialAtlas &materials) {
	// The set-0 cache keys on these RIDs, so the next render rebuilds with the new arrays.
	material_albedo_ = materials.albedo_array();
	material_surface_ = materials.surface_array();
	material_sampler_ = materials.sampler();
}

void RaymarchPass::set_sun_ubo(RID buffer) {
	// The set-2 cache keys on this RID.
	sun_ubo_ = buffer;
}

void RaymarchPass::teardown() {
	if (!rd_) return;
	// uset_mask_ is only a cache key for an externally owned tile-mask RID (usually the
	// IslandAtlas fallback mask); it is not registered and never freed here.
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_ = sampler_linear_ = edits_ubo_ = RID();
	albedo_ = surface_ = hitpos_ = cost_buf_ = RID();
	set_ = sun_set_ = gpu::SetCache();
	uset_mask_ = RID();
	sun_ubo_ = RID();
	material_albedo_ = RID();
	material_surface_ = RID();
	material_sampler_ = RID();
	rd_ = nullptr;
}

void RaymarchPass::rebuild_targets(RenderingDevice *rd, int w, int h) {
	// The old targets and cost buffer take set 0 with them (device cascade); its cache
	// rebuilds on the new RIDs.
	gpu::RdDevice device{rd};
	for (RID *r : {&albedo_, &surface_, &hitpos_, &cost_buf_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const uint32_t usage = RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	albedo_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM, Vector2i(w, h), usage);
	surface_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, Vector2i(w, h), usage);
	hitpos_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, Vector2i(w, h), usage);
	cost_buf_ = group_.add(gpu::Kind::Buffer,
			rd->storage_buffer_create(static_cast<uint32_t>(w) * h * 2u * sizeof(uint32_t)));
	width_ = w;
	height_ = h;
}

std::vector<gpu::Uniform> RaymarchPass::uniforms(const GpuAtlas &atlas, const IslandAtlas &islands,
		RID mask) const {
	return {
		gpu::image(0, albedo_),
		gpu::image(1, hitpos_),
		// Binding 2 is the R8_UNORM SDF atlas and is the only one filtered in hardware.
		gpu::sampled(2, sampler_linear_, atlas.sdf_atlas()),
		gpu::sampled(3, sampler_, atlas.mat_atlas()),
		gpu::sampled(4, sampler_, atlas.mip_atlas(0)),
		gpu::sampled(5, sampler_, atlas.mip_atlas(1)),
		gpu::sampled(6, sampler_, atlas.mip_atlas(2)),
		gpu::storage(7, atlas.palette()),
		gpu::storage(8, atlas.region_map()),
		gpu::storage(9, atlas.region_tables()),
		gpu::storage(10, atlas.op_pool()),
		gpu::storage(11, atlas.op_counts()),
		gpu::ubo(12, edits_ubo_),
		// 13-17: shared authoritative volume SDF/material (indexed by Island.volume_slot since
		// Task 6), island min-max chain, descriptors, tile mask. Atlas slot still selects the
		// descriptor/mip/tile-mask entries.
		gpu::storage(13, atlas.volumes().sdf_buffer()),
		gpu::storage(14, atlas.volumes().mat_buffer()),
		gpu::storage(15, islands.mip_buffer()),
		gpu::storage(16, islands.desc_buffer()),
		gpu::storage(17, mask),
		// 18-19: the shared material arrays (set by set_materials()).
		gpu::sampled(18, material_sampler_, material_albedo_),
		gpu::sampled(19, material_sampler_, material_surface_),
		gpu::image(20, surface_),
		gpu::storage(21, atlas.brick_flags()),
		gpu::storage(22, atlas.region_slot_counts()),
		gpu::storage(23, cost_buf_),
		// 24-26: the compact-normal pool -- packed payload plus BOTH offset tables (per volume
		// slot, per override-brick slot). -1 in a table row means "no normals bound".
		gpu::storage(24, atlas.stored_normals().normal_buffer()),
		gpu::storage(25, atlas.stored_normals().volume_offsets_buffer()),
		gpu::storage(26, atlas.stored_normals().override_offsets_buffer()),
		// 27-30: the shared authoritative override pool (SDF bytes, material bytes, brick
		// tables, region-to-table map) the field evaluator consults for shading normals.
		gpu::storage(27, atlas.overrides().sdf_buffer()),
		gpu::storage(28, atlas.overrides().mat_buffer()),
		gpu::storage(29, atlas.overrides().tables()),
		gpu::storage(30, atlas.overrides().region_table_map()),
	};
}

bool RaymarchPass::targets_need_rebuild(int width, int height, RID mask) const {
	return width != width_ || height != height_ || mask != uset_mask_ ||
			(rd_ && !rd_->uniform_set_is_valid(set_.id()));
}

bool RaymarchPass::render(RenderingDevice *rd, const GpuAtlas &atlas,
		const IslandAtlas *islands, RID tile_mask, const ve::CameraParams &cam,
		int width, int height, const float edit_state[6],
		const FieldContextSet *field_context) {
	if (!program_.valid()) return false;
	if (!islands || !islands->is_valid()) return false;
	const RID mask = tile_mask.is_valid() ? tile_mask : islands->fallback_mask();
	if (width != width_ || height != height_ || mask != uset_mask_ ||
			!rd->uniform_set_is_valid(set_.id())) {
		rebuild_targets(rd, width, height);
		uset_mask_ = mask;
	}
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, program_.shader, 0, uniforms(atlas, *islands, mask));
	const RID sun_set = sun_set_.get(device, group_, program_.shader, 2, {gpu::ubo(24, sun_ubo_)});
	if (!set.is_valid() || !sun_set.is_valid() || !albedo_.is_valid() || !surface_.is_valid() ||
			!edits_ubo_.is_valid()) return false;

	// Recorded before the compute list: buffer_update errors while a list is open, and the
	// deferred update still lands before the dispatch at submit.
	{
		const ve::EditsBlock edits{{edit_state[0], edit_state[1], edit_state[2], 0.0f},
				{edit_state[3] /* radius */, edit_state[4] /* type */, edit_state[5] /* material */,
						edit_state[3] > 0.0f ? 1.0f : 0.0f}};
		rd->buffer_update(edits_ubo_, 0, sizeof(edits), gpu::push_bytes(edits));
	}

	const PackedByteArray pc = gpu::push_bytes(cam);

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set, 0);
	rd->compute_list_bind_uniform_set(list, sun_set, 2);
	if (field_context != nullptr) field_context->bind(rd, list);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (width + kRaymarchGroupX - 1) / kRaymarchGroupX,
			(height + kRaymarchGroupY - 1) / kRaymarchGroupY, 1);
	rd->compute_list_end();
	return true;
}
