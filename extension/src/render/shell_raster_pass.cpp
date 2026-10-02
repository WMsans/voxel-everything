#include "render/shell_raster_pass.h"
#include "render/gbuffer.h"
#include "render/lod_pool.h"
#include "gpu_layout/blocks.h"
#include "lod/lod_contour.h"
#include <godot_cpp/variant/packed_color_array.hpp>
#include <algorithm>

using namespace godot;

ShellRasterPass::~ShellRasterPass() {
	teardown();
}

void ShellRasterPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return;
	rd_ = rd;
	thick_shader_ = gpu::compile_raster(rd, group_, "ShellRasterPass", "shell.vert.glsl",
			"shell_thickness.frag.glsl");
	front_shader_ = gpu::compile_raster(rd, group_, "ShellRasterPass", "shell.vert.glsl",
			"shell_front.frag.glsl");
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!thick_shader_.is_valid() || !front_shader_.is_valid()) teardown();
}

void ShellRasterPass::release_targets() {
	if (!rd_) return;
	thick_fb_.release(rd_, group_);
	front_fb_.release(rd_, group_);
}

void ShellRasterPass::teardown() {
	if (!rd_) return;
	gpu::RdDevice device{rd_};
	group_.release(device);
	thick_shader_ = front_shader_ = thick_pipeline_ = front_pipeline_ = sampler_ = RID();
	thick_ = front_ = depth_ = args_ = RID();
	thick_set_ = front_set_ = gpu::SetCache();
	thick_fb_ = front_fb_ = gpu::FramebufferCache();
	size_ = Vector2i(0, 0);
	args_capacity_ = 0;
	drew_ = false;
	rd_ = nullptr;
}

bool ShellRasterPass::ensure_targets(RenderingDevice *rd, Vector2i size) {
	if (size.x <= 0 || size.y <= 0) return false;
	if (size == size_ && thick_.is_valid() && front_.is_valid() && depth_.is_valid()) return true;
	thick_fb_.release(rd, group_);
	front_fb_.release(rd, group_);
	gpu::RdDevice device{rd};
	for (RID *r : {&thick_, &front_, &depth_}) {
		group_.free(device, *r);
		*r = RID();
	}
	const uint32_t colour = RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT;
	thick_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32_SFLOAT, size, colour);
	front_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_R32G32B32A32_SFLOAT, size, colour);
	depth_ = gpu::texture(rd, group_, RenderingDevice::DATA_FORMAT_D32_SFLOAT, size,
			RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	size_ = size;
	return thick_.is_valid() && front_.is_valid() && depth_.is_valid();
}

bool ShellRasterPass::ensure_args(RenderingDevice *rd, int pages) {
	// ponytail: the indirect arg buffer only ever grows; a smaller page count just uploads
	// fewer draws into it. Shrink to size if a scene's peak page count turns out to be large.
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

bool ShellRasterPass::draw(RenderingDevice *rd, LodPool &pool, RID index_array, GBuffer &gb,
		RID beauty_cam_ubo, RID island_desc, float fade_start, float fade_end,
		bool front_face_clockwise, bool camera_inside) {
	drew_ = false;
	if (!rd_ || rd != rd_ || !thick_shader_.is_valid() || !front_shader_.is_valid() || !gb.is_valid())
		return false;
	// ponytail: an ice-free frame leaves the targets stale and drew() false, and the composite
	// skips them, so no clear is paid for. Clear here instead if anything ever reads them stale.
	if (pages_.empty() && !camera_inside) return true;
	if (!index_array.is_valid() || !beauty_cam_ubo.is_valid() || !island_desc.is_valid()) return false;
	if (!ensure_targets(rd, gb.size()) ||
			!ensure_args(rd, std::max(1, static_cast<int>(pages_.size()))))
		return false;
	if (!thick_fb_.get(rd, group_, {thick_, gb.depth()}).is_valid()) return false;
	if (!front_fb_.get(rd, group_, {front_, depth_}).is_valid()) return false;
	if (!thick_pipeline_.is_valid() || !front_pipeline_.is_valid() ||
			pipeline_clockwise_ != front_face_clockwise) {
		gpu::RdDevice device{rd};
		group_.free(device, thick_pipeline_);
		group_.free(device, front_pipeline_);
		// The winding LodRasterPass MEASURED (M5 errata 2) decides gl_FrontFacing in the
		// thickness pass and the culled side in the front pass.
		const RenderingDevice::PolygonFrontFace winding = front_face_clockwise
				? RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE
				: RenderingDevice::POLYGON_FRONT_FACE_COUNTER_CLOCKWISE;
		gpu::RasterState thick;
		thick.cull = RenderingDevice::POLYGON_CULL_DISABLED;
		thick.front = winding;
		thick.depth_write = false; // the G-buffer's depth is tested, never written
		thick.color_attachments = 1;
		thick.additive = true;
		thick_pipeline_ = gpu::raster_pipeline(rd, group_, thick_shader_, thick_fb_.format(), thick);
		gpu::RasterState front;
		front.cull = RenderingDevice::POLYGON_CULL_BACK;
		front.front = winding;
		front.color_attachments = 1;
		front_pipeline_ = gpu::raster_pipeline(rd, group_, front_shader_, front_fb_.format(), front);
		pipeline_clockwise_ = front_face_clockwise;
	}
	if (!thick_pipeline_.is_valid() || !front_pipeline_.is_valid()) return false;
	gpu::RdDevice device{rd};
	const RID thick_set = thick_set_.get(device, group_, thick_shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::storage(5, pool.normal_buffer()),
			gpu::ubo(7, beauty_cam_ubo),
			gpu::storage(8, island_desc)});
	const RID front_set = front_set_.get(device, group_, front_shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::storage(5, pool.normal_buffer()),
			gpu::sampled(6, sampler_, gb.depth()),
			gpu::ubo(7, beauty_cam_ubo),
			gpu::storage(8, island_desc)});
	if (!thick_set.is_valid() || !front_set.is_valid()) return false;

	// Device-level upload before any draw list opens; same command layout as
	// LodPool::upload_draw_args.
	if (!pages_.empty()) {
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
	}
	const ve::ShellRasterPush push{{fade_start, fade_end, 0.0f, 0.0f}};
	const uint32_t count = static_cast<uint32_t>(pages_.size());

	// Thickness: colour cleared, the G-buffer depth loaded and kept. A camera inside the
	// medium starts every pixel with one front at distance 0: (R, G) = (0, +1).
	PackedColorArray thick_clear;
	thick_clear.push_back(Color(0.0f, camera_inside ? 1.0f : 0.0f, 0.0f, 0.0f));
	int64_t dl = rd->draw_list_begin(thick_fb_.rid(), RenderingDevice::DRAW_CLEAR_COLOR_ALL,
			thick_clear);
	if (dl < 0) return false;
	if (count > 0) {
		rd->draw_list_bind_render_pipeline(dl, thick_pipeline_);
		rd->draw_list_bind_uniform_set(dl, thick_set, 0);
		rd->draw_list_bind_index_array(dl, index_array);
		rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
		rd->draw_list_draw_indirect(dl, true, args_, 0, count, 20);
	}
	rd->draw_list_end();

	PackedColorArray front_clear;
	front_clear.push_back(Color(0, 0, 0, 0));
	dl = rd->draw_list_begin(front_fb_.rid(),
			RenderingDevice::DRAW_CLEAR_COLOR_ALL | RenderingDevice::DRAW_CLEAR_DEPTH, front_clear, 0.0f);
	if (dl < 0) return false;
	if (count > 0) {
		rd->draw_list_bind_render_pipeline(dl, front_pipeline_);
		rd->draw_list_bind_uniform_set(dl, front_set, 0);
		rd->draw_list_bind_index_array(dl, index_array);
		rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
		rd->draw_list_draw_indirect(dl, true, args_, 0, count, 20);
	}
	rd->draw_list_end();
	drew_ = true;
	return true;
}
