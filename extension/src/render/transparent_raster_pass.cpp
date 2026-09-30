#include "render/transparent_raster_pass.h"
#include "render/gbuffer.h"
#include "render/lod_pool.h"
#include "gpu_layout/blocks.h"
#include "lod/lod_contour.h"
#include <godot_cpp/variant/packed_color_array.hpp>
#include <algorithm>

using namespace godot;

TransparentRasterPass::~TransparentRasterPass() {
	teardown();
}

void TransparentRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	shader_ = gpu::compile_raster(rd, group_, "TransparentRasterPass", "transparent.vert.glsl",
			"transparent.frag.glsl");
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!shader_.is_valid()) teardown();
}

void TransparentRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	shader_ = pipeline_ = sampler_ = RID();
	front_ = trans_ = depth_ = args_ = RID();
	set_ = gpu::SetCache();
	framebuffer_ = gpu::FramebufferCache();
	size_ = Vector2i(0, 0);
	args_capacity_ = 0;
	drew_ = false;
	rd_ = nullptr;
}

bool TransparentRasterPass::ensure_targets(RenderingDevice *rd, Vector2i size) {
	if (size == size_ && front_.is_valid() && trans_.is_valid() && depth_.is_valid()) return true;
	framebuffer_.release(rd, group_);
	gpu::RdDevice device{rd};
	for (RID *r : {&front_, &trans_, &depth_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const uint32_t colour = RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	front_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, size, colour);
	trans_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, size, colour);
	depth_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_D32_SFLOAT, size,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
	size_ = size;
	return front_.is_valid() && trans_.is_valid() && depth_.is_valid();
}

bool TransparentRasterPass::ensure_args(RenderingDevice *rd, int pages) {
	if (pages <= args_capacity_ && args_.is_valid()) return true;
	gpu::RdDevice device{rd};
	group_.free(device, args_);
	args_capacity_ = std::max(64, pages * 2);
	PackedByteArray zero;
	zero.resize(static_cast<int64_t>(args_capacity_) * 20);
	zero.fill(0);
	args_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(
			static_cast<uint32_t>(zero.size()), zero, RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT));
	return args_.is_valid();
}

bool TransparentRasterPass::draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
		RID beauty_cam_ubo, float fade_start, float fade_end, bool front_face_clockwise) {
	drew_ = false;
	if (!rd_ || rd != rd_ || !shader_.is_valid() || !gb.is_valid()) return false;
	if (pages_.empty()) return true;
	if (!index_array.is_valid() || !beauty_cam_ubo.is_valid()) return false;
	if (!ensure_targets(rd, gb.size()) || !ensure_args(rd, static_cast<int>(pages_.size()))) return false;
	if (!framebuffer_.get(rd, group_, {front_, trans_, depth_}).is_valid()) return false;
	if (!pipeline_.is_valid() || pipeline_clockwise_ != front_face_clockwise) {
		gpu::RdDevice device{rd};
		group_.free(device, pipeline_);
		gpu::RasterState state;
		// Front faces only, with the winding LodRasterPass MEASURED (M5 errata 2); the
		// nearest shell fragment wins on this pass's own reverse-Z depth.
		state.cull = RenderingDevice::POLYGON_CULL_BACK;
		state.front = front_face_clockwise ? RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE
				: RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
		state.color_attachments = 2;
		pipeline_ = gpu::raster_pipeline(rd, group_, shader_, framebuffer_.format(), state);
		pipeline_clockwise_ = front_face_clockwise;
	}
	if (!pipeline_.is_valid()) return false;
	gpu::RdDevice device{rd};
	const RID set = set_.get(device, group_, shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::storage(5, pool.normal_buffer()),
			gpu::sampled(6, sampler_, gb.depth()),
			gpu::ubo(7, beauty_cam_ubo)});
	if (!set.is_valid()) return false;

	// Device-level upload before the draw list opens (M2 Task 12's ordering rule); same
	// command layout as LodPool::upload_draw_args.
	PackedByteArray args;
	args.resize(static_cast<int64_t>(pages_.size()) * 20);
	uint32_t *a = reinterpret_cast<uint32_t *>(args.ptrw());
	for (size_t i = 0; i < pages_.size(); i++) {
		a[i * 5 + 0] = static_cast<uint32_t>(pages_[i].quad_count * 6);
		a[i * 5 + 1] = 1u;
		a[i * 5 + 2] = 0u;
		a[i * 5 + 3] = static_cast<uint32_t>(pages_[i].page * ve::kLodVertsPerPage);
		a[i * 5 + 4] = 0u;
	}
	rd->buffer_update(args_, 0, args.size(), args);

	PackedColorArray clears;
	clears.push_back(Color(0, 0, 0, 0));
	clears.push_back(Color(0, 0, 0, 0));
	const int64_t dl = rd->draw_list_begin(framebuffer_.rid(),
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH, clears, 0.0f);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set, 0);
	rd->draw_list_bind_index_array(dl, index_array);
	const ve::TransparentRasterPush push{{fade_start, fade_end, 0.0f, 0.0f}};
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	rd->draw_list_draw_indirect(dl, true, args_, 0, static_cast<uint32_t>(pages_.size()), 20);
	rd->draw_list_end();
	drew_ = true;
	return true;
}
