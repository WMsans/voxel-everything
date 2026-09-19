#include "debug/hooks.h"

#include "../voxel_world.h"
#include "render/frame.h"
#include "render/frame_params.h"
#include "render/orchestrator.h"
#include "settings/godot/setting_variant.h"
#include "terrain/field_params_pack.h"
#include <cstring>
#include "mesh/consolidation.h"
#include "render/gpu_atlas.h"
#include "render/material_atlas.h"
#include "render/camera_params.h"
#include "render/island_atlas.h"
#include "render/island_cull_pass.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/deferred_pass.h"
#include "render/inject_pass.h"
#include "render/gbuffer.h"
#include "render/beauty_camera.h"
#include "render/contact_shadow_pass.h"
#include "render/ssgi_pass.h"
#include "render/ssao_pass.h"
#include "render/ssr_pass.h"
#include "render/outline_pass.h"
#include "beauty_compositor.h"
#include "render/region_pass.h"
#include "render/brick_gen_pass.h"
#include "render/world_streamer.h"
#include "render/shader_loader.h"
#include "render/mesh_pass.h"
#include "render/mesh_service.h"
#include "render/field_context_set.h"
#include "render/lod_build_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/sun_ubo.h"
#include "render/lod_cull_pass.h"
#include "render/grass_scatter_pass.h"
#include "render/leaf_scatter_pass.h"
#include "render/leaf_raster_pass.h"
#include "render/grass_raster_pass.h"
#include "grass/grass_layout.h"
#include "render/hiz_pass.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "lod/lod_skirt.h"
#include "lod/lod_system.h" // Task 15: the LoD state/mutex live here; friend access
#include "lod/lod_tree.h"
#include "physics/collider_streamer.h"
#include "physics/island_manager.h"
#include "mesh/dual_contour.h"
#include "mesh/mesh_chunk.h"
#include "mesh/box_merge.h"
#include "generator/generator.h"
#include "world/brick_eval.h"
#include "world/brick_flags.h"
#include "world/brick_mip.h"
#include "world/raycast.h"
#include "shade/oct.h"
#include "shade/cel.h"
#include "shade/sun_cascades.h"
#include "shade/sun_ortho.h"
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <set>
#include <vector>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>

#include "debug/hooks_common.h"

namespace godot {

Dictionary VoxelDebugHooks::debug_gpu_timings() {
	return world_->context().render->gpu_timings()->snapshot();
}

Dictionary VoxelDebugHooks::debug_ingest_gpu_timings(const PackedStringArray &names,
		const PackedInt64Array &gpu_us, int64_t rd_frame) {
	return world_->context().render->gpu_timings()->ingest_for_test(names, gpu_us, static_cast<uint64_t>(rd_frame));
}

Dictionary VoxelDebugHooks::debug_beauty_compositor_stats() {
	Dictionary d;
	d["normal_roughness"] = world_->context().render->normal_roughness_state();
	d["contact_ms"] = world_->context().render->passes().contact_shadow ? world_->context().render->passes().contact_shadow->last_ms() : 0.0f;
	// CPU command-record time only; GPU timings belong to the later performance task.
	d["ssr_ms"] = world_->context().render->passes().ssr ? world_->context().render->passes().ssr->last_ms() : 0.0f;
	d["outline_ms"] = world_->context().render->passes().outline ? world_->context().render->passes().outline->last_ms() : 0.0f;
	return d;
}

Dictionary VoxelDebugHooks::debug_contact_shadow_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["mask_width"] = 0; d["mask_height"] = 0;
	d["mask_min"] = 1.0f; d["mask_mean"] = 1.0f;
	d["mean_darkening"] = 0.0f; d["max_brightening"] = 0.0f;
	d["max_neighbour_step"] = 0.0f;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().contact_shadow) return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	// Both halves of the shipped frame, split at the opaque boundary so scene colour can be
	// captured between them: `before` is what inject left, the scene colour afterwards is what
	// the post-opaque stages (contact shadows, then SSR and outlines) made of it.
	VoxelFrame *frame = &world_->context().render->frame();
	const FrameInputs in = frame->prepare_headless(device, VoxelFrame::looking_at(pos, fwd, w, h));
	if (!in.scene_color.is_valid()) return d;
	frame->render_pre_opaque(device, in);
	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
	tf->set_width(w);
	tf->set_height(h);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	const RID before = device->texture_create(tf, tv, {});
	if (!before.is_valid()) return d;
	device->texture_copy(in.scene_color, before, Vector3(), Vector3(), Vector3(w, h, 1), 0, 0, 0, 0);
	frame->render_post_opaque(device, in);
	device->submit();
	device->sync();
	if (!frame->last_frame().stage_ok(kStageContact)) {
		device->free_rid(before);
		return d;
	}
	const int mw = std::max(1, w / 2), mh = std::max(1, h / 2);
	d["mask_width"] = mw; d["mask_height"] = mh;
	PackedByteArray mask;
	if (world_->context().render->passes().contact_shadow->mask().is_valid())
		mask = device->texture_get_data(world_->context().render->passes().contact_shadow->mask(), 0);
	const PackedByteArray pre = device->texture_get_data(before, 0);
	const PackedByteArray post = device->texture_get_data(in.scene_color, 0);
	if (mask.size() >= mw * mh && pre.size() >= w * h * 8 && post.size() >= w * h * 8) {
		const uint8_t *m = reinterpret_cast<const uint8_t *>(mask.ptr());
		const uint16_t *a = reinterpret_cast<const uint16_t *>(pre.ptr());
		const uint16_t *b = reinterpret_cast<const uint16_t *>(post.ptr());
		float min_mask = 1.0f; double mask_sum = 0.0, dark = 0.0; float bright = 0.0f;
		for (int i = 0; i < mw * mh; i++) { const float v = m[i] / 255.0f; min_mask = std::min(min_mask, v); mask_sum += v; }
		// Per-pixel FRACTION of light removed, so the speckle measure below is independent
		// of how bright the surface under the shadow happens to be.
		std::vector<float> removed(static_cast<size_t>(w) * h, 0.0f);
		for (int i = 0; i < w * h; i++) {
			const float la = 0.2126f * Math::half_to_float(a[i * 4]) + 0.7152f * Math::half_to_float(a[i * 4 + 1]) + 0.0722f * Math::half_to_float(a[i * 4 + 2]);
			const float lb = 0.2126f * Math::half_to_float(b[i * 4]) + 0.7152f * Math::half_to_float(b[i * 4 + 1]) + 0.0722f * Math::half_to_float(b[i * 4 + 2]);
			dark += std::max(0.0f, la - lb); bright = std::max(bright, lb - la);
			removed[i] = la > 1e-4f ? std::clamp((la - lb) / la, 0.0f, 1.0f) : 0.0f;
		}
		// The largest jump in that fraction between neighbouring pixels. A resolved shadow
		// is a gradient; an unresolved bayer4 dither steps the full strength in one pixel.
		float step = 0.0f;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				const float c = removed[static_cast<size_t>(y) * w + x];
				if (x + 1 < w) step = std::max(step, std::fabs(c - removed[static_cast<size_t>(y) * w + x + 1]));
				if (y + 1 < h) step = std::max(step, std::fabs(c - removed[static_cast<size_t>(y + 1) * w + x]));
			}
		d["mask_min"] = min_mask; d["mask_mean"] = static_cast<float>(mask_sum / (mw * mh));
		d["mean_darkening"] = static_cast<float>(dark / (w * h)); d["max_brightening"] = bright;
		d["max_neighbour_step"] = step;
	}
	device->free_rid(before);
	return d;
}


Dictionary VoxelDebugHooks::debug_ssgi_probe(Vector3 pos, Vector3 fwd, int w, int h, int frames) {
	Dictionary d;
	d["width"] = std::max(1, w / 2);
	d["height"] = std::max(1, h / 2);
	d["max_channel"] = 0.0f;
	d["mean_luma"] = 0.0;
	d["ran"] = false;
	if (w <= 0 || h <= 0 || frames <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer || !world_->context().render->passes().ssgi)
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	// `frames` consecutive shipped frames of one view. History, the previous view-projection
	// and the temporal frame index are whatever the frame recorded at the end of the last one.
	bool ran = false;
	const FrameInputs in = VoxelFrame::looking_at(pos, fwd, w, h);
	for (int i = 0; i < frames; i++) {
		world_->context().render->frame().render_headless(device, in);
		device->submit();
		device->sync();
		ran = ran || world_->context().render->frame().last_frame().stage_ok(kStageSsgi);
	}
	d["ran"] = ran;
	const RID output = world_->context().render->passes().ssgi->result();
	const Vector2i half = world_->context().render->passes().gbuffer->half_size();
	d["width"] = half.x;
	d["height"] = half.y;
	if (!output.is_valid()) return d;
	const PackedByteArray data = device->texture_get_data(output, 0);
	const int pixels = half.x * half.y;
	if (data.size() < static_cast<int64_t>(pixels) * 8) return d;
	const uint16_t *values = reinterpret_cast<const uint16_t *>(data.ptr());
	float max_channel = 0.0f;
	double mean_luma = 0.0;
	std::vector<float> luma(static_cast<size_t>(pixels));
	for (int i = 0; i < pixels; i++) {
		const float r = Math::half_to_float(values[i * 4]);
		const float g = Math::half_to_float(values[i * 4 + 1]);
		const float b = Math::half_to_float(values[i * 4 + 2]);
		max_channel = std::max(max_channel, std::max(r, std::max(g, b)));
		luma[static_cast<size_t>(i)] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
		mean_luma += luma[static_cast<size_t>(i)];
	}
	d["max_channel"] = max_channel;
	d["mean_luma"] = mean_luma / static_cast<double>(pixels);
	// How much of the result is a 4x4 lattice. Every pixel is compared with the box over one
	// full bayer4 period around it (offsets -1..2, the same window the contact-shadow resolve
	// uses): a resolved gather is a gradient and barely differs from that box, while an
	// unresolved per-pixel rotation puts each pixel's own sampling luck on screen and differs
	// from it by a large fraction of the light itself.
	double deviation = 0.0, level = 0.0;
	for (int y = 1; y + 2 < half.y; y++)
		for (int x = 1; x + 2 < half.x; x++) {
			float box = 0.0f;
			for (int j = -1; j <= 2; j++)
				for (int i = -1; i <= 2; i++) box += luma[static_cast<size_t>(y + j) * half.x + x + i];
			box *= 1.0f / 16.0f;
			deviation += std::fabs(luma[static_cast<size_t>(y) * half.x + x] - box);
			level += box;
		}
	d["lattice_ratio"] = level > 1e-6 ? deviation / level : 0.0;
	return d;
}

// One frame of raymarch -> composite -> HBAO over the G-buffer, then a readback of the
// single-channel occlusion target. Reads the AO texture directly: whether the deferred pass
// applies it is deferred's own contract, not this probe's.
Dictionary VoxelDebugHooks::debug_ssao_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["width"] = w;
	d["height"] = h;
	d["min_ao"] = 1.0f;
	d["max_ao"] = 0.0f;
	d["ran"] = false;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer || !world_->context().render->passes().ssao)
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	// The shipped frame, headless: SSAO runs only when the beauty flags give it work, over the
	// G-buffer the frame composited at near_field_scale, marched to the fade band.
	world_->context().render->frame().render_headless(device, VoxelFrame::looking_at(pos, fwd, w, h));
	device->submit();
	device->sync();
	const bool ran = world_->context().render->frame().last_frame().stage_ok(kStageSsao);
	d["ran"] = ran;
	{
		const PackedByteArray lit = device->texture_get_data(world_->context().render->passes().gbuffer->lit(), 0);
		const int pixels = w * h;
		if (lit.size() >= static_cast<int64_t>(pixels) * 8) {
			const uint16_t *lv = reinterpret_cast<const uint16_t *>(lit.ptr());
			double luma = 0.0;
			for (int i = 0; i < pixels; i++) {
				const float r = Math::half_to_float(lv[i * 4]);
				const float g = Math::half_to_float(lv[i * 4 + 1]);
				const float b = Math::half_to_float(lv[i * 4 + 2]);
				luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
			}
			d["lit_luma"] = luma / static_cast<double>(pixels);
		} else {
			d["lit_luma"] = -1.0;
		}
	}
	const RID output = world_->context().render->passes().ssao->result();
	if (!output.is_valid()) return d;
	const PackedByteArray data = device->texture_get_data(output, 0);
	// The AO target is half the G-buffer, so it is not w*h. Read back its own size and
	// report it, rather than assuming the probe's requested dimensions.
	const Vector2i ao_size = world_->context().render->passes().ssao->size();
	const int pixels = ao_size.x * ao_size.y;
	d["ao_width"] = ao_size.x;
	d["ao_height"] = ao_size.y;
	if (pixels <= 0 || data.size() < pixels) return d;
	const uint8_t *values = reinterpret_cast<const uint8_t *>(data.ptr());
	float min_ao = 1.0f, max_ao = 0.0f;
	double mean_ao = 0.0;
	for (int i = 0; i < pixels; i++) {
		const float ao = static_cast<float>(values[i]) / 255.0f;
		min_ao = std::min(min_ao, ao);
		max_ao = std::max(max_ao, ao);
		mean_ao += ao;
	}
	d["min_ao"] = min_ao;
	d["max_ao"] = max_ao;
	d["mean_ao"] = mean_ao / static_cast<double>(pixels);
	return d;
}

// A runtime render-scale change reconfigures the viewport, which makes the engine drop the
// voxel_gbuf context; GBuffer::ensure() then recreates `history` with UNDEFINED contents.
// This probe reproduces that reallocation and reports whether has_history() still claims a
// history exists. If it does, SSGI bounces whatever was left in that memory.
Dictionary VoxelDebugHooks::debug_ssgi_history_latch_probe(int w, int h, int w2, int h2) {
	Dictionary d;
	d["ran"] = false;
	d["after_write"] = false;
	d["reallocated"] = false;
	d["after_realloc"] = true;
	if (w <= 0 || h <= 0 || w2 <= 0 || h2 <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer) return d;
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(w, h))) return d;
	// The production path writes the history at the end of every frame; do the same once so
	// the latch is genuinely set before the reallocation.
	if (!world_->context().render->downsample_history(device, world_->context().render->passes().gbuffer->lit(), *world_->context().render->passes().gbuffer)) return d;
	d["after_write"] = world_->context().render->has_history();
	const int before = world_->context().render->passes().gbuffer->reallocations();
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(w2, h2))) return d;
	d["reallocated"] = world_->context().render->passes().gbuffer->reallocations() > before;
	d["after_realloc"] = world_->context().render->has_history();
	d["ran"] = true;
	return d;
}

Dictionary VoxelDebugHooks::debug_ssgi_reprojection_probe(Vector3 previous_pos, Vector3 previous_fwd,
		Vector3 current_pos, Vector3 current_fwd, int w, int h) {
	Dictionary d;
	d["non_identity"] = previous_pos != current_pos || previous_fwd != current_fwd;
	d["mapping_luma"] = 0.0;
	d["current_mapping_luma"] = 0.0;
	d["mapping_delta"] = 0.0;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer || !world_->context().render->passes().ssgi)
		return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(previous_pos) == 0 ? quiet + 1 : 0;
	// The frame reprojects SSGI's history through the view-projection it recorded at the end of
	// the PREVIOUS frame. Frame 1 at the previous camera leaves that matrix and a history; frame
	// 2 at the current camera therefore gathers through the previous mapping; frame 3 at the
	// same current camera gathers through the current mapping. Their difference is what a
	// broken reprojection would erase.
	auto render = [&](Vector3 camera_pos, Vector3 camera_fwd) {
		world_->context().render->frame().render_headless(device, VoxelFrame::looking_at(camera_pos, camera_fwd, w, h));
		device->submit();
		device->sync();
		return world_->context().render->frame().last_frame().stage_ok(kStageSsgi);
	};
	auto read_luma = [&]() {
		const Vector2i half = world_->context().render->passes().gbuffer->half_size();
		const PackedByteArray data = device->texture_get_data(world_->context().render->passes().ssgi->result(), 0);
		const int pixels = half.x * half.y;
		if (data.size() < static_cast<int64_t>(pixels) * 8) return 0.0;
		const uint16_t *values = reinterpret_cast<const uint16_t *>(data.ptr());
		double luma = 0.0;
		for (int i = 0; i < pixels; i++)
			luma += 0.2126 * Math::half_to_float(values[i * 4]) +
					0.7152 * Math::half_to_float(values[i * 4 + 1]) +
					0.0722 * Math::half_to_float(values[i * 4 + 2]);
		return luma / static_cast<double>(pixels);
	};
	if (!render(previous_pos, previous_fwd)) return d;
	if (!render(current_pos, current_fwd)) return d;
	const double mapping_luma = read_luma();
	if (!render(current_pos, current_fwd)) return d;
	const double current_mapping_luma = read_luma();
	d["mapping_luma"] = mapping_luma;
	d["current_mapping_luma"] = current_mapping_luma;
	d["mapping_delta"] = std::fabs(mapping_luma - current_mapping_luma);
	return d;
}

Dictionary VoxelDebugHooks::debug_beauty_settings() {
	ve::BeautySettings beauty;
	int quality_tier;
	world_->context().render->beauty_snapshot(&beauty, &quality_tier);

	// Every row by name, so a new beauty knob appears here without another line.
	Dictionary d;
	for (const ve::SettingRow<ve::BeautySettings> &row : ve::beauty_rows())
		d[row.name] = setting_to_variant(ve::read(row, beauty));
	d["islands"] = world_->get_effect_enabled("islands");
	d["tier"] = quality_tier;
	d["flags"] = static_cast<int>(ve::pack_beauty_flags(beauty));
	return d;
}

Dictionary VoxelDebugHooks::debug_stored_normal_stats() {
	Dictionary d;
	if (!world_->context().render->passes().atlas || !world_->context().render->passes().atlas->is_valid()) return d;
	const StoredNormalStats s = world_->context().render->passes().atlas->stored_normals().stats();
	d["capacity_bytes"] = static_cast<int64_t>(s.capacity_bytes);
	d["live_bytes"] = static_cast<int64_t>(s.live_bytes);
	d["high_water_bytes"] = static_cast<int64_t>(s.high_water_bytes);
	d["allocation_failures"] = static_cast<int64_t>(s.allocation_failures);
	d["fallback_hits"] = static_cast<int64_t>(s.fallback_hits);
	// Task 8: the exact telemetry keys the HUD and the teardown/telemetry tests read.
	d["normal_capacity_bytes"] = static_cast<int64_t>(s.capacity_bytes);
	d["normal_live_bytes"] = static_cast<int64_t>(s.live_bytes);
	d["normal_high_water_bytes"] = static_cast<int64_t>(s.high_water_bytes);
	d["normal_allocation_failures"] = static_cast<int64_t>(s.allocation_failures);
	d["normal_fallback_hits"] = static_cast<int64_t>(s.fallback_hits);
	return d;
}

Dictionary VoxelDebugHooks::debug_normal_pool_state() {
	Dictionary d;
	const bool have_pool = world_->context().render->passes().atlas && world_->context().render->passes().atlas->is_valid();
	const StoredNormalPool *p = have_pool ? &world_->context().render->passes().atlas->stored_normals() : nullptr;
	d["pool_valid"] = p && p->is_valid();
	d["normal_rid_valid"] = p && p->normal_buffer().is_valid();
	d["volume_offsets_rid_valid"] = p && p->volume_offsets_buffer().is_valid();
	d["override_offsets_rid_valid"] = p && p->override_offsets_buffer().is_valid();
	d["volume_offsets_all_minus_one"] =
			p && p->is_valid() && p->volume_offsets_all_minus_one();
	d["override_offsets_all_minus_one"] =
			p && p->is_valid() && p->override_offsets_all_minus_one();
	return d;
}

int64_t VoxelDebugHooks::debug_normal_upload_override(int slot,
		const PackedByteArray &packed_normals) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas || !world_->context().render->passes().atlas->is_valid()) return -1;
	if (slot < 0) return -1;
	if (packed_normals.is_empty() || packed_normals.size() % 2 != 0) {
		// Malformed payload: exercise the pool's fallback path rather than uploading junk.
		return world_->context().render->passes().atlas->stored_normals().upload_override(device, slot, nullptr, 0);
	}
	return world_->context().render->passes().atlas->stored_normals().upload_override(device, slot,
			reinterpret_cast<const uint16_t *>(packed_normals.ptr()),
			static_cast<int>(packed_normals.size() / 2));
}

void VoxelDebugHooks::debug_normal_release_override(int slot) {
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().atlas || !world_->context().render->passes().atlas->is_valid()) return;
	world_->context().render->passes().atlas->stored_normals().release_override(device, slot);
}

Dictionary VoxelDebugHooks::debug_leaf_stats() {
	Dictionary d;
	d["ran"] = false;
	d["capacity"] = 0;
	d["trees"] = 0;
	d["clumps"] = 0;
	d["high_water"] = 0;
	// Stage-2 sample keys, same "0 means not measured" convention as debug_grass_stats.
	d["sampled"] = 0;
	d["max_crown_offset"] = 0.0;
	// Raster key, same convention as debug_grass_stats': the vertex count the SHIPPING
	// raster last recorded (six per clump), never a CPU re-derivation.
	d["vertices"] = 0;
	VoxelWorld *w = world_;
	if (!w) return d;
	LeafScatterPass *l = w->context().render->passes().leaf_scatter;
	if (!l) return d;
	// Local-device worlds drive the SHIPPING pass on demand (the debug_grass_stats pattern:
	// same calls, same order as the compositor block). Test bodies run synchronously with
	// no compositor frame, so a pure read would report stale zeros forever; running the
	// real pass is not a parallel scatter. Demo worlds stay pure-read -- the compositor
	// owns the frame there.
	if (w->get_use_local_device()) {
		w->ensure_initialized();
		RenderingDevice *device = w->rd();
		GpuAtlas *atlas = w->context().render->passes().atlas;
		if (!w->is_initialized() || !device || !atlas || !atlas->is_valid()) return d;
		// Hook camera: 40 m straight above the last streamed centre (every leaf test
		// streams before reading), looking straight down. The lift is load-bearing: the
		// test view sits at crown height, and a camera AT canopy level frustum-clips most
		// crowns, turning the reach comparison into a noise fight. 90-degree FOV, time
		// and sun handling match the compositor expression, as with grass.
		const float *c = w->context().store->center_;
		const float p[3] = {c[0], c[1] + 40.0f, c[2]};
		const float f[3] = {0.0f, -1.0f, 0.0f};
		const ve::ProbeCamera pc = ve::probe_camera(p, f, 64, 64,
				1.5707963268f, 0.1f, 4000.0f);
		const ve::LodCamera &cam = pc.lod;
		float vp[16];
		for (int k = 0; k < 16; k++) vp[k] = cam.view_proj[k];
		const ve::LeafLayout ll = w->context().render->frame().leaf_layout(p, vp);
		// Stage 2's sun march reads the SunUbo; stage 1 never does. The hook ensures the
		// same buffer the compositor hands the pass, so the drive exercises both stages.
		if (!w->context().render->passes().sun_ubo || !w->context().render->passes().sun_ubo->ensure(device)) return d;
		if (!l->run(device, *atlas, ll, w->context().store->region_window(),
				static_cast<float>(w->context().render->beauty_frame()) / 60.0f,
				w->context().render->passes().sun_ubo->buffer(),
				w->context().render->passes().field_context)) return d;
		// run()'s internal readback lands before the dispatch executes; the counters are
		// only valid after a submit+sync, which the compositor does at frame end and the
		// hook must do itself before refreshing through the pass's re-read entry point.
		device->submit();
		device->sync();
		l->read_back_counters(device);
		l->read_back_sample(device);
		// The raster counter is only fresh if the SHIPPING raster ran too: same drive, same
		// hook camera, into the owned probe-size G-buffer (the debug_grass_stats pattern).
		// last_vertex_count() is CPU-side, but the recorded draw is submitted so the device
		// never holds an unsubmitted list. A hooked clear first: the raster's own
		// draw_list_begin performs no clear, so without the LoD pass's clear_targets the
		// reverse-Z compare would test against whatever depth the last probe left behind.
		LeafRasterPass *leaf_raster = w->context().render->passes().leaf_raster;
		if (leaf_raster && w->context().render->passes().gbuffer &&
				w->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(64, 64))) {
			Projection view_proj;
			for (int cc = 0; cc < 4; cc++)
				for (int rr = 0; rr < 4; rr++) view_proj.columns[cc][rr] = vp[cc * 4 + rr];
			if (w->context().render->passes().lod_raster)
				w->context().render->passes().lod_raster->clear_targets(device, *w->context().render->passes().gbuffer);
			leaf_raster->draw(device, *l, *w->context().render->passes().gbuffer, view_proj, p);
			device->submit();
			device->sync();
		}
	}
	// Re-read for the report: demo worlds skip the drive above (the compositor owns the
	// frame there), so fetch the passes here for the pure-read keys.
	d["ran"] = true;
	d["capacity"] = l->capacity();
	d["trees"] = l->last_tree_count();
	d["clumps"] = l->last_clump_count();
	d["high_water"] = l->clump_high_water();
	d["sampled"] = l->sample_count();
	d["max_crown_offset"] = l->sample_max_crown_offset();
	{
		LeafRasterPass *r = w->context().render->passes().leaf_raster;
		d["vertices"] = r ? r->last_vertex_count() : 0;
	}
	return d;
}

Dictionary VoxelDebugHooks::debug_grass_stats() {
	Dictionary d;
	d["ran"] = false;
	d["bricks"] = 0;
	d["blades"] = 0;
	d["capacity"] = 0;
	d["high_water"] = 0;
	d["sampled"] = 0;
	d["min_normal_y"] = 1.0;
	d["max_height"] = 0.0;
	d["min_sun"] = 1.0;
	d["max_sun"] = 0.0;
	d["mean_sun"] = 0.0;
	d["drawn"] = false;
	d["vertices"] = 0;
	// Shading observables, filled by the hooked drive below. -1.0 is the honest
	// "not measured" sentinel (the ssao probe's lit_luma fallback): demo worlds stay
	// pure-read and never reach the drive, so they report these defaults.
	d["min_luma"] = -1.0;
	d["max_luma"] = -1.0;
	d["mean_luma"] = -1.0;
	// The material ids the hooked blade raster wrote into surface.z, ascending. Material 0 is
	// the cleared background, so only covered pixels report. S7 pins this: a blade must write
	// its own foliage id, never the terrain material it grows on.
	d["blade_materials"] = PackedInt32Array();
	VoxelWorld *w = world_;
	if (!w) return d;
	GrassScatterPass *g = w->context().render->passes().grass_scatter;
	if (!g) return d;
	// Null until ensure_initialized() builds the graph inside the drive below; assigned
	// there and re-read after for the report keys.
	GrassRasterPass *raster = nullptr;
	// Local-device worlds drive the SHIPPING pass on demand (the debug_ssao_probe pattern:
	// same calls, same order as the compositor block). Test bodies run synchronously with
	// no compositor frame, so a pure read would report stale zeros forever; running the
	// real pass is not a parallel scatter. Demo worlds stay pure-read -- the compositor
	// owns the frame there.
	if (w->get_use_local_device()) {
		w->ensure_initialized();
		RenderingDevice *device = w->rd();
		GpuAtlas *atlas = w->context().render->passes().atlas;
		if (!w->is_initialized() || !device || !atlas || !atlas->is_valid()) return d;
		// Hook camera: the last streamed centre (every grass test streams before reading),
		// looking straight down. 90-degree FOV so the reach comparison measures the
		// box/distance cull, not the test frustum. Time matches the compositor expression.
		const float *c = w->context().store->center_;
		const float p[3] = {c[0], c[1], c[2]};
		const float f[3] = {0.0f, -1.0f, 0.0f};
		const ve::ProbeCamera pc = ve::probe_camera(p, f, 64, 64,
				1.5707963268f, 0.1f, 4000.0f);
		const ve::LodCamera &cam = pc.lod;
		float vp[16];
		for (int k = 0; k < 16; k++) vp[k] = cam.view_proj[k];
		const ve::GrassLayout gl = w->context().render->frame().grass_layout(p, vp);
		// w->rd() above already published this world's sun into the SunUbo, the same
		// buffer the compositor hands the pass.
		if (!w->context().render->passes().sun_ubo || !w->context().render->passes().sun_ubo->ensure(device)) return d;
		if (!g->run(device, *atlas, gl, w->context().store->region_window(),
				static_cast<float>(w->context().render->beauty_frame()) / 60.0f,
				w->context().render->passes().sun_ubo->buffer(),
				w->context().render->passes().field_context)) return d;
		// run()'s internal readback lands before the dispatch executes; the counters are
		// only valid after a submit+sync, which the compositor does at frame end and the
		// hook must do itself before refreshing through the pass's re-read entry point.
		device->submit();
		device->sync();
		g->read_back_counters(device);
		g->read_back_sample(device);
		// The raster counter is only fresh if the SHIPPING raster ran too: same drive,
		// same hook camera, into the owned probe-size G-buffer (the debug_gbuffer_stats
		// pattern). last_vertex_count() is CPU-side, but the recorded draw is submitted
		// so the device never holds an unsubmitted list. Fetched here, after
		// ensure_initialized(): the graph (and the raster with it) may not have existed
		// on entry.
		raster = w->context().render->passes().grass_raster;
		if (raster && w->context().render->passes().gbuffer &&
				w->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(64, 64))) {
			Projection view_proj;
			for (int cc = 0; cc < 4; cc++)
				for (int rr = 0; rr < 4; rr++) view_proj.columns[cc][rr] = vp[cc * 4 + rr];
			// Fixed background: the raster's own draw_list_begin(DRAW_DEFAULT_ALL)
			// performs no clear -- verified against LodRasterPass::draw, which opens its
				// list the same way and relies on a separate explicit clear (production
				// grass relies on the compositor's earlier clears and draws over the LoD
				// frame, unchanged). So the HOOK clears the owned 64x64 targets through a
				// render pass before drawing. Via the LoD pass's clear_targets, not a
				// texture_clear: a colour clear is refused on the depth format (see
				// debug_lod_render_probe). Albedo/surface go to (0,0,0,0), depth to 0.0
				// (reverse-Z far).
			if (w->context().render->passes().lod_raster)
				w->context().render->passes().lod_raster->clear_targets(device, *w->context().render->passes().gbuffer);
			raster->draw(device, *g, *w->context().render->passes().gbuffer, view_proj, p);
			device->submit();
			device->sync();
			// Shading observable: min/max/mean luma of the albedo the hooked raster
				// just drew, with the ssao probe's 0.2126/0.7152/0.0722 weights. Albedo
				// is R8G8B8A8_UNORM, so plain bytes, not halves.
			const PackedByteArray alb = device->texture_get_data(w->context().render->passes().gbuffer->albedo(), 0);
			const int pixels = 64 * 64;
			if (alb.size() >= pixels * 4) {
				const uint8_t *a = reinterpret_cast<const uint8_t *>(alb.ptr());
				float mn = 1.0f, mx = 0.0f;
				double sum = 0.0;
				for (int i = 0; i < pixels; i++) {
					const float luma = (0.2126f * a[i * 4] + 0.7152f * a[i * 4 + 1] +
							0.0722f * a[i * 4 + 2]) / 255.0f;
					mn = std::min(mn, luma);
					mx = std::max(mx, luma);
					sum += luma;
				}
				d["min_luma"] = mn;
				d["max_luma"] = mx;
				d["mean_luma"] = sum / static_cast<double>(pixels);
			}
			const PackedByteArray surf = device->texture_get_data(
					w->context().render->passes().gbuffer->surface(), 0);
			if (surf.size() >= pixels * 8) {
				const uint16_t *s = reinterpret_cast<const uint16_t *>(surf.ptr());
				std::set<int> seen;
				for (int i = 0; i < pixels; i++) {
					const float z = half_to_float(s[i * 4 + 2]);
					if (z >= 0.5f) seen.insert(static_cast<int>(z + 0.5f));
				}
				PackedInt32Array ids;
				for (int id : seen) ids.push_back(id);
				d["blade_materials"] = ids;
			}
		}
	}
	// Re-read for the report: demo worlds skip the drive above (the compositor owns the
	// frame there), so fetch the pass here for the pure-read keys.
	raster = w->context().render->passes().grass_raster;
	d["ran"] = true;
	d["bricks"] = g->last_brick_count();
	d["blades"] = g->last_blade_count();
	d["capacity"] = g->capacity();
	d["high_water"] = g->blade_high_water();
	d["sampled"] = g->sample_count();
	d["min_normal_y"] = g->sample_min_normal_y();
	d["max_height"] = g->sample_max_height();
	d["min_sun"] = g->sample_min_sun();
	d["max_sun"] = g->sample_max_sun();
	d["mean_sun"] = g->sample_mean_sun();
	d["drawn"] = raster != nullptr;
	d["vertices"] = raster ? raster->last_vertex_count() : 0;
	return d;
}

Dictionary VoxelDebugHooks::debug_gbuffer_stats(int w, int h) {
	Dictionary d;
	d["valid"] = false;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer) return d;
	// The probe path: no RenderSceneBuffersRD exists outside a render callback, so this
	// exercises the owned branch. Everything else about the object is identical.
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(w, h))) {
		d["reallocations"] = world_->context().render->passes().gbuffer->reallocations();
		return d;
	}
	d["valid"] = world_->context().render->passes().gbuffer->is_valid();
	d["width"] = world_->context().render->passes().gbuffer->size().x;
	d["height"] = world_->context().render->passes().gbuffer->size().y;
	d["half_width"] = world_->context().render->passes().gbuffer->half_size().x;
	d["half_height"] = world_->context().render->passes().gbuffer->half_size().y;
	d["albedo_valid"] = world_->context().render->passes().gbuffer->albedo().is_valid();
	d["surface_valid"] = world_->context().render->passes().gbuffer->surface().is_valid();
	d["depth_valid"] = world_->context().render->passes().gbuffer->depth().is_valid();
	d["lit_valid"] = world_->context().render->passes().gbuffer->lit().is_valid();
	d["history_valid"] = world_->context().render->passes().gbuffer->history().is_valid();
	d["albedo_id"] = static_cast<int64_t>(world_->context().render->passes().gbuffer->albedo().get_id());
	d["depth_id"] = static_cast<int64_t>(world_->context().render->passes().gbuffer->depth().get_id());
	d["reallocations"] = world_->context().render->passes().gbuffer->reallocations();
	return d;
}

Dictionary VoxelDebugHooks::debug_hiz_stats() {
	Dictionary d;
	world_->ensure_initialized();
	if (!world_->context().render->passes().hiz || !world_->rd()) return d;
	d["width"] = HizPass::kSize;
	d["height"] = HizPass::kSize;
	d["mips"] = HizPass::kMipCount;
	d["readback_level"] = HizPass::kReadbackLevel;
	d["readback_texels"] = HizPass::kGrid * HizPass::kGrid;
	return d;
}

Dictionary VoxelDebugHooks::debug_hiz_shutdown_probe() {
	Dictionary d;
	d["callback_guarded"] = false;
	d["queued"] = false;
	d["was_pending"] = false;
	d["drained"] = false;
	d["initialized_after"] = true;
	{
		const bool callback_guarded = world_->try_begin_render_callback();
		d["callback_guarded"] = callback_guarded;
		if (!callback_guarded) return d;
		struct CallbackGuard {
			VoxelWorld *world;
			~CallbackGuard() { world->end_render_callback(); }
		} callback_guard{world_};
		world_->ensure_initialized();
		RenderingDevice *device = world_->rd();
		if (!device || !world_->context().render->passes().hiz || !world_->context().render->passes().gbuffer) return d;
		const Vector2i size(64, 64);
		if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, size)) return d;
		if (!world_->context().render->passes().hiz->build(device, world_->context().render->passes().gbuffer->depth(), size)) return d;
		d["queued"] = world_->context().render->passes().hiz->readback_pending();
	}
	world_->shutdown_render_resources();
	d["was_pending"] = world_->context().render->last_hiz_readback_was_pending();
	d["drained"] = world_->context().render->last_hiz_readback_was_drained();
	d["initialized_after"] = world_->is_initialized();
	return d;
}

Dictionary VoxelDebugHooks::debug_hiz_probe_synthetic(float far_value, float near_value) {
	Dictionary d;
	d["mip0_at_near_texel"] = 0.0f;
	d["mip1_covering_both"] = 0.0f;
	d["top_mip"] = 0.0f;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().hiz) return d;

	// A 256^2 synthetic depth image: every texel is `far_value` except one near texel at
	// (0,0). With the level-0 pass mapping the scene 1:1 at this size, mip 0 keeps the near
	// value, the mip-1 parent over the 2x2 corner keeps the far value, and the 1x1 top mip
	// keeps the far value too.
	const int size = HizPass::kSize;
	PackedByteArray data;
	data.resize(size * size * 4);
	float *pixels = reinterpret_cast<float *>(data.ptrw());
	for (int i = 0; i < size * size; i++) pixels[i] = far_value;
	pixels[0] = near_value;

	TypedArray<PackedByteArray> upload;
	upload.push_back(data);
	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
	tf->set_width(size);
	tf->set_height(size);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	const RID synthetic = device->texture_create(tf, tv, upload);
	if (!synthetic.is_valid()) return d;

	if (world_->context().render->passes().hiz->build(device, synthetic, Vector2i(size, size))) {
		device->submit();
		device->sync();
		d["mip0_at_near_texel"] = world_->context().render->passes().hiz->probe_mip_texel(device, 0, 0, 0);
		d["mip1_covering_both"] = world_->context().render->passes().hiz->probe_mip_texel(device, 1, 0, 0);
		d["top_mip"] = world_->context().render->passes().hiz->probe_mip_texel(device, HizPass::kMipCount - 1, 0, 0);
		// Make the async readback deterministic for the test hooks: read the 4 KB copy
		// synchronously after sync and feed it into the same occlusion grid the walk uses.
		const PackedByteArray rb = device->texture_get_data(world_->context().render->passes().hiz->readback_texture(), 0);
		world_->context().render->passes().hiz->update_occlusion(rb);
	}
	// The level-0 uniform set references this throwaway source; drop the cached set before
	// freeing the texture so the next probe does not try to free a cascade-freed set.
	world_->context().render->passes().hiz->release_level0_set();
	device->free_rid(synthetic);
	return d;
}

bool VoxelDebugHooks::debug_hiz_occluded(Vector2 lo, Vector2 hi, float depth) {
	world_->ensure_initialized();
	if (!world_->context().render->passes().hiz || !world_->rd()) return false;
	const float ss_min[3] = {lo.x, lo.y, depth};
	const float ss_max[3] = {hi.x, hi.y, depth};
	return world_->context().render->passes().hiz->occlusion()->occluded(ss_min, ss_max);
}

PackedInt32Array VoxelDebugHooks::debug_island_tile_mask(Vector3 origin, Vector3 dir, float tan_x,
		float tan_y, int width, int height) {
	PackedInt32Array out;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().islands || !world_->context().render->passes().island_cull) return out;
	ve::CameraParams cam = ve::CameraParams::looking_at(origin.x, origin.y, origin.z,
			dir.x, dir.y, dir.z, 0, 1, 0);
	// looking_at leaves the tangents at 0 (the 1x1 probes need no frustum); a cull test does.
	cam.params[0] = tan_x;
	cam.params[1] = tan_y;
	if (!world_->context().render->passes().island_cull->render(device, *world_->context().render->passes().islands, cam, width, height,
				std::max(world_->context().render->island_slot_count(), 1)))
		return out;
	device->submit();
	device->sync();
	const int n = world_->context().render->passes().island_cull->tiles_x() * world_->context().render->passes().island_cull->tiles_y();
	const PackedByteArray b = device->buffer_get_data(world_->context().render->passes().island_cull->mask_buffer(), 0,
			static_cast<uint32_t>(n) * 4);
	if (b.size() < static_cast<int64_t>(n) * 4) return out;
	out.resize(n);
	std::memcpy(out.ptrw(), b.ptr(), static_cast<size_t>(n) * 4);
	return out;
}

bool VoxelDebugHooks::render_probe_pixel(Vector3 origin, Vector3 dir) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch)
		return false;
	// The probe is a read-only diagnostic: it must not mutate the streamed world.
	const Vector3 forward = dir.normalized();
	const float f[3] = {forward.x, forward.y, forward.z};
	float up[3];
	ve::probe_up_hint(f, up);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, f[0], f[1], f[2], up[0], up[1], up[2]);
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cam, ve::pack_flags(world_->context().render->beauty_settings()));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(),
			cam, 1, 1, kNoEdit, world_->context().render->passes().field_context)) return false;
	device->submit();
	device->sync();
	return true;
}

Color VoxelDebugHooks::debug_raymarch_pixel(Vector3 origin, Vector3 dir) {
	if (!render_probe_pixel(origin, dir)) return Color(1, 0, 1);
	RenderingDevice *device = world_->rd();
	const PackedByteArray data = device->texture_get_data(world_->context().render->passes().raymarch->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (data.size() < 4 || sf.size() < 8 || hp.size() < 16) return Color(1, 0, 1);
	const uint8_t *b = data.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	// The marcher's albedo target is the ray OVERLAY now, not a colour. Resolve the material
	// the way the composite will, so "what colour is this pixel" keeps its old answer.
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float n[3];
	ve::oct_decode(e, n);
	const Color c = resolve_near_field(static_cast<int>(half_to_float(s[2]) + 0.5f),
			Vector3(hf[0], hf[1], hf[2]), Vector3(n[0], n[1], n[2]),
			Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1.0f), half_to_float(s[3]), nullptr);
	// Alpha stays the HIT FLAG, as every existing caller assumes -- the albedo image's own
	// alpha is sun visibility and would read as "missed" for any shadowed pixel.
	return Color(c.r, c.g, c.b, hf[3]);
}

Dictionary VoxelDebugHooks::debug_raymarch_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	if (!render_probe_pixel(origin, dir)) return d;
	RenderingDevice *device = world_->rd();
	const PackedByteArray hp = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	const PackedByteArray col = device->texture_get_data(world_->context().render->passes().raymarch->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	if (hp.size() < 16 || col.size() < 4 || sf.size() < 8) return d;
	const float *hf = reinterpret_cast<const float *>(hp.ptr());
	const uint8_t *b = col.ptr();
	const uint16_t *sv = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float oct_e[2] = {half_to_float(sv[0]), half_to_float(sv[1])};
	float nrm[3];
	ve::oct_decode(oct_e, nrm);
	const int hit_mat = static_cast<int>(half_to_float(sv[2]) + 0.5f);
	d["material"] = hit_mat;
	d["normal"] = Vector3(nrm[0], nrm[1], nrm[2]);
	// `color` is what the composite resolves for this pixel, not the marcher's overlay target
	// -- the marcher stopped resolving materials when that moved to full resolution.
	d["color"] = resolve_near_field(hit_mat, Vector3(hf[0], hf[1], hf[2]),
			Vector3(nrm[0], nrm[1], nrm[2]),
			Color(b[0] / 255.0f, b[1] / 255.0f, b[2] / 255.0f, 1.0f), half_to_float(sv[3]), nullptr);
	if (hf[3] < 0.5f) return d; // sky miss
	d["hit"] = true;
	d["pos"] = Vector3(hf[0], hf[1], hf[2]);
	const ve::IVec3 brick = ve::brick_of_point(hf[0], hf[1], hf[2]);
	d["brick"] = Vector3i(brick.x, brick.y, brick.z);
	// Reproduce the shader's lookups on the CPU to report what the mips said there.
	const ve::IVec3 rs_region = ve::region_of_brick(brick);
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
	const ve::IVec3 abv = world_->context().render->passes().atlas->config().atlas_bricks;
	const ve::IVec3 cell{slot % abv.x, (slot / abv.x) % abv.y, slot / (abv.x * abv.y)};
	const PackedByteArray m2 = device->texture_get_data(world_->context().render->passes().atlas->mip_atlas(0), 0);
	const PackedByteArray m8 = device->texture_get_data(world_->context().render->passes().atlas->mip_atlas(2), 0);
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

Dictionary VoxelDebugHooks::debug_raymarch_cost_probe(Vector3 origin, Vector3 dir) {
	Dictionary out;
	out["hit"] = false;
	out["steps"] = 0;
	out["bricks"] = 0;
	out["regions"] = 0;
	world_->ensure_initialized();
	if (!world_->is_initialized()) return out;
	if (!render_probe_pixel(origin, dir)) return out;
	RenderingDevice *device = world_->rd();
	const PackedByteArray words = device->buffer_get_data(world_->context().render->passes().raymarch->cost_buffer(), 0, 8);
	if (words.size() < 8) return out;
	const uint32_t steps = words.decode_u32(0);
	const uint32_t cells = words.decode_u32(4);
	const PackedByteArray hp = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (hp.size() >= 16) {
		const float *hf = reinterpret_cast<const float *>(hp.ptr());
		out["hit"] = hf[3] > 0.5f;
	}
	out["steps"] = static_cast<int>(steps);
	out["bricks"] = static_cast<int>(cells & 0xFFFFu);
	out["regions"] = static_cast<int>(cells >> 16);
	return out;
}

Dictionary VoxelDebugHooks::debug_raymarch_gbuffer(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch) return d;
	const Vector3 forward = dir.normalized();
	const float f[3] = {forward.x, forward.y, forward.z};
	float up[3];
	ve::probe_up_hint(f, up);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, f[0], f[1], f[2], up[0], up[1], up[2]);
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cam, ve::pack_flags(world_->context().render->beauty_settings()));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cam, 1, 1, kNoEdit, world_->context().render->passes().field_context)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(world_->context().render->passes().raymarch->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["sun"] = a[3] / 255.0f;
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float n[3];
	ve::oct_decode(e, n);
	d["normal"] = Vector3(n[0], n[1], n[2]);
	const int mat = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["material"] = mat;
	d["hit"] = h[3] > 0.5f;
	d["position"] = Vector3(h[0], h[1], h[2]);
	// What the marcher actually stored: the ray overlay and the weight the composite mixes it
	// with. On an ordinary hit that is (0,0,0) at weight 0 -- the whole pixel is the material.
	const Color overlay(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["overlay"] = overlay;
	d["overlay_weight"] = half_to_float(s[3]);
	// ...and what the composite resolves from it. `albedo` and `gloss` are G-BUFFER values,
	// which is where they are produced now; this reproduces that resolve for one pixel.
	float gloss = 0.0f;
	d["albedo"] = resolve_near_field(mat, Vector3(h[0], h[1], h[2]), Vector3(n[0], n[1], n[2]),
			overlay, half_to_float(s[3]), &gloss);
	d["gloss"] = gloss;
	return d;
}

// Isolated g-buffer holes: a pixel the primary march missed while all four of its
// neighbours hit. Real sky is a connected region, so an isolated miss in the middle of
// terrain can only be the march stepping over geometry it should have found. Counting
// them is view-robust in a way that naming one guilty pixel is not.
//
// Task 7 (ruling R9): thin elevated trunks broke the premise, not the renderer. Each
// isolated miss is therefore adjudicated from this hook's own field and residency data,
// and only two field-true classes are exempt -- (1) sub-pixel sky at silhouettes: the
// CPU raycast crosses a voxel column, but sampling the ANALYTIC field at 0.25 m along
// the exact ray (crossing -2 m .. +12 m, the measured window) never goes solid, so the
// marcher and the field agree the ray is sky; (2) rays landing past the funding
// frontier: the crossing's region has no atlas slot (slot_of < 0) and the marcher
// crosses unfunded regions as "known empty" by design (raymarch.comp.glsl header).
// Any other miss -- field-solid on the exact ray inside funded terrain -- is
// unexplained march tunneling and must still fail. docs/superpowers/specs/
// 2026-09-18-trees-design.md §10 pre-accepts the silhouette/LoD-boundary residue.
Dictionary VoxelDebugHooks::debug_raymarch_hole_probe(Vector3 origin, Vector3 dir, int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hit_pixels"] = 0;
	d["isolated_misses"] = 0;
	d["isolated_exempt_field_true_sky"] = 0;
	d["isolated_exempt_past_funding_frontier"] = 0;
	d["isolated_unexplained"] = 0;
	if (w <= 2 || h <= 2) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch) return d;
	const float p[3] = {origin.x, origin.y, origin.z};
	const float basis_f[3] = {dir.x, dir.y, dir.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, basis_f, w, h,
			1.0471975512f, 0.05f, 4000.0f);
	ve::CameraParams cam{};
	for (int axis = 0; axis < 3; ++axis) {
		cam.cam_pos[axis] = p[axis];
		cam.cam_right[axis] = pc.right[axis];
		cam.cam_up[axis] = pc.up[axis];
		cam.cam_fwd[axis] = pc.fwd[axis];
	}
	cam.params[0] = pc.tan_x;
	cam.params[1] = pc.tan_y;
	cam.params[2] = 200.0f;
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cam, ve::pack_flags(world_->context().render->beauty_settings()));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cam, w, h, kNoEdit, world_->context().render->passes().field_context)) return d;
	device->submit();
	device->sync();
	const PackedByteArray hp = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (hp.size() < static_cast<int64_t>(w) * h * 16) return d;
	const float *f = reinterpret_cast<const float *>(hp.ptr());
	std::vector<uint8_t> hit(static_cast<size_t>(w) * h, 0);
	int hits = 0;
	for (int i = 0; i < w * h; i++) {
		hit[i] = f[i * 4 + 3] > 0.5f ? 1 : 0;
		hits += hit[i];
	}
	int isolated = 0, sky = 0, frontier = 0, unexplained = 0;
	Array miss_details;
	const ve::Generator *gen = world_->context().store->generator();
	for (int y = 1; y < h - 1; y++)
		for (int x = 1; x < w - 1; x++) {
			const size_t i = static_cast<size_t>(y) * w + x;
			if (hit[i]) continue;
			if (!(hit[i - 1] && hit[i + 1] && hit[i - w] && hit[i + w])) continue;
			isolated++;
			Dictionary m;
			m["x"] = x;
			m["y"] = y;
			// Reconstruct the pixel's exact ray from the same camera basis the probe used.
			const float ndc_x = (static_cast<float>(x) + 0.5f) * 2.0f / static_cast<float>(w) - 1.0f;
			const float ndc_y = 1.0f - (static_cast<float>(y) + 0.5f) * 2.0f / static_cast<float>(h);
			Vector3 rd(basis_f[0] + pc.right[0] * ndc_x * pc.tan_x + pc.up[0] * ndc_y * pc.tan_y,
					basis_f[1] + pc.right[1] * ndc_x * pc.tan_x + pc.up[1] * ndc_y * pc.tan_y,
					basis_f[2] + pc.right[2] * ndc_x * pc.tan_x + pc.up[2] * ndc_y * pc.tan_y);
			rd = rd.normalized();
			const Dictionary r = debug_raycast(origin, rd);
			if (!bool(r.get("hit", false))) {
				sky++;
				m["class"] = "sky_no_cpu_hit";
				miss_details.append(m);
				continue;
			}
			const Vector3 hitp = r["pos"];
			const float d0 = origin.distance_to(hitp);
			m["cpu_distance"] = d0;
			m["cpu_material"] = int(r["material"]);
			const ve::IVec3 reg = ve::region_of_point(hitp.x, hitp.y, hitp.z);
			const int slot = debug_slot_of_region(Vector3i(reg.x, reg.y, reg.z));
			m["slot"] = slot;
			if (slot < 0) {
				frontier++;
				m["class"] = "past_funding_frontier";
				miss_details.append(m);
				continue;
			}
			// Analytic-field truth at the crossing (0.25 m steps, -2 m .. +12 m window).
			float min_sdf = 1e30f;
			if (gen != nullptr) {
				int n0 = static_cast<int>((d0 - 2.0f) / 0.25f);
				if (n0 < 0) n0 = 0;
				for (int s = n0; static_cast<float>(s) * 0.25f < d0 + 12.0f; s++) {
					const Vector3 q = origin + rd * (static_cast<float>(s) * 0.25f);
					const ve::IVec3 qr = ve::region_of_point(q.x, q.y, q.z);
					std::vector<ve::EditOp> ops;
					{
						std::lock_guard<std::mutex> lock(world_->context().store->edit_mutex());
						if (world_->context().store->edit_log())
							ops = world_->context().store->edit_log()->ops(qr);
					}
					const ve::Sample sample = ve::eval_field(*gen, ops.data(),
							static_cast<int>(ops.size()), q.x, q.y, q.z,
							&world_->context().store->volumes(), world_->context().store->overrides());
					if (sample.sdf < min_sdf) min_sdf = sample.sdf;
				}
				m["min_sdf"] = min_sdf;
			}
			if (gen != nullptr && min_sdf >= 0.0f) {
				sky++;
				m["class"] = "sub_pixel_silhouette_sky";
			} else {
				unexplained++;
				m["class"] = "UNEXPLAINED";
			}
			miss_details.append(m);
		}
	d["ran"] = true;
	d["hit_pixels"] = hits;
	d["isolated_misses"] = isolated;
	d["isolated_exempt_field_true_sky"] = sky;
	d["isolated_exempt_past_funding_frontier"] = frontier;
	d["isolated_unexplained"] = unexplained;
	d["isolated_miss_details"] = miss_details;
	return d;
}

Dictionary VoxelDebugHooks::debug_raymarch_normal_probe(Vector3 origin, Vector3 dir, int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hits"] = 0;
	d["rms_ndl"] = 0.0;
	d["cel_mismatch_fraction"] = 0.0;
	d["largest_mismatch_component"] = 0;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch) return d;
	const float p[3] = {origin.x, origin.y, origin.z};
	const float f[3] = {dir.x, dir.y, dir.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, f, w, h,
			1.0471975512f, 0.05f, 4000.0f);
	ve::CameraParams cam{};
	for (int axis = 0; axis < 3; ++axis) {
		cam.cam_pos[axis] = p[axis];
		cam.cam_right[axis] = pc.right[axis];
		cam.cam_up[axis] = pc.up[axis];
		cam.cam_fwd[axis] = pc.fwd[axis];
	}
	cam.params[0] = pc.tan_x;
	cam.params[1] = pc.tan_y;
	cam.params[2] = 200.0f;
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cam, ve::pack_flags(world_->context().render->beauty_settings()));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cam, w, h, kNoEdit, world_->context().render->passes().field_context)) return d;
	device->submit();
	device->sync();
	const PackedByteArray surface = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	const PackedByteArray hitpos = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (surface.size() < static_cast<int64_t>(w) * h * 8) return d;
	if (hitpos.size() < static_cast<int64_t>(w) * h * 16) return d;
	const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
	const float *hp = reinterpret_cast<const float *>(hitpos.ptr());
	constexpr float kSun[3] = {0.5746958f, 0.7662610f, 0.2873479f};
	constexpr float kEdges[3] = {0.08f, 0.32f, 0.66f};
	auto band = [](float ndl) { return ndl > 0.66f ? 3 : ndl > 0.32f ? 2 : ndl > 0.08f ? 1 : 0; };
	int total_hits = 0;
	for (int i = 0; i < w * h; i++) if (hp[i * 4 + 3] > 0.5f) total_hits++;
	std::vector<uint8_t> mismatch(static_cast<size_t>(w) * h, 0);
	double sum_sq = 0.0;
	int considered = 0;
	int mismatches = 0;
	// Task 7: the reference normal comes from the CPU field evaluator over the hit
	// point's own region op span, consulting the same VolumeSet / OverrideStore the
	// GPU's authoritative buffers mirror -- not from an inline analytic formula, so
	// edits, stored volumes and consolidated overrides are all covered. For a pure
	// procedural hit this reduces exactly to Task 1's analytic gradient.
	const ve::Generator &gen = *world_->context().store->generator();
	std::lock_guard<std::mutex> edit_lock(world_->context().store->edit_mutex());
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int i = y * w + x;
			if (hp[i * 4 + 3] <= 0.5f) continue;
			// Decode hit normal from surface texture.
			const float e[2] = {half_to_float(s[i * 4 + 0]), half_to_float(s[i * 4 + 1])};
			float rn[3];
			ve::oct_decode(e, rn);
			float rlen = std::sqrt(rn[0]*rn[0] + rn[1]*rn[1] + rn[2]*rn[2]);
			if (rlen > 1e-8f) { rn[0]/=rlen; rn[1]/=rlen; rn[2]/=rlen; }
			const float hitx = hp[i * 4 + 0];
			const float hity = hp[i * 4 + 1];
			const float hitz = hp[i * 4 + 2];
			// CPU reference gradient over this region's op span.
			const std::vector<ve::EditOp> &ops =
					world_->context().store->edit_log()->ops(ve::region_of_point(hitx, hity, hitz));
			const ve::FieldSample fs = ve::eval_field_gradient(gen, ops.data(),
					static_cast<int>(ops.size()), hitx, hity, hitz, &world_->context().store->volumes(), world_->context().store->overrides());
			if (!fs.exact_gradient) continue;
			const float gx = fs.gradient[0], gy = fs.gradient[1], gz = fs.gradient[2];
			float alen = std::sqrt(gx*gx + gy*gy + gz*gz);
			if (alen < 1e-8f) continue;
			float an[3] = {gx/alen, gy/alen, gz/alen};
			float ndl_render = rn[0]*kSun[0] + rn[1]*kSun[1] + rn[2]*kSun[2];
			float ndl_analytic = an[0]*kSun[0] + an[1]*kSun[1] + an[2]*kSun[2];
			double diff = double(ndl_render) - double(ndl_analytic);
			sum_sq += diff * diff;
			considered++;
			bool far_from_edges = true;
			for (int eidx = 0; eidx < 3; eidx++) if (std::fabs(ndl_analytic - kEdges[eidx]) < 0.01f) far_from_edges = false;
			if (!far_from_edges) continue;
			if (band(ndl_render) != band(ndl_analytic)) {
				mismatch[static_cast<size_t>(i)] = 1;
				mismatches++;
			}
		}
	}
	double rms = considered > 0 ? std::sqrt(sum_sq / double(considered)) : 0.0;
	double frac = considered > 0 ? double(mismatches) / double(considered) : 0.0;
	int largest = 0;
	std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
	std::vector<int> stack;
	stack.reserve(1024);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int idx = y * w + x;
			if (!mismatch[static_cast<size_t>(idx)] || visited[static_cast<size_t>(idx)]) continue;
			int comp = 0;
			stack.clear();
			stack.push_back(idx);
			visited[static_cast<size_t>(idx)] = 1;
			while (!stack.empty()) {
				int cur = stack.back(); stack.pop_back();
				comp++;
				int cx = cur % w;
				int cy = cur / w;
				if (cx > 0) { int nb = cur - 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cx + 1 < w) { int nb = cur + 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy > 0) { int nb = cur - w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy + 1 < h) { int nb = cur + w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
			}
			if (comp > largest) largest = comp;
		}
	}
	d["ran"] = true;
	d["hits"] = total_hits;
	d["considered"] = considered; // hits whose CPU reference gradient was exact and compared
	d["rms_ndl"] = rms;
	d["cel_mismatch_fraction"] = frac;
	d["largest_mismatch_component"] = largest;
	return d;
}

// Task 7: the island counterpart of debug_raymarch_normal_probe. Every hit inside the
// body's local lattice box is compared against a trilinear blend of that body's OWN
// compact normals (VolumeData::normal_oct, CPU authoritative), rotated into the world by
// the body basis -- the same numbers the shader decodes at an island hit. Same five
// metrics as the terrain probe.
Dictionary VoxelDebugHooks::debug_island_normal_probe(int island_slot, Vector3 origin, Vector3 dir,
		int w, int h) {
	Dictionary d;
	d["ran"] = false;
	d["hits"] = 0;
	d["rms_ndl"] = 0.0;
	d["cel_mismatch_fraction"] = 0.0;
	d["largest_mismatch_component"] = 0;
	if (w <= 0 || h <= 0 || island_slot < 0 || island_slot >= kMaxIslands) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch) return d;
	// The descriptor the SHADER sees is the one on the device: test-placed islands are
	// uploaded directly, so read it back rather than trusting any cached copy.
	const int64_t desc_bytes = static_cast<int64_t>(kMaxIslands) * 128;
	const PackedByteArray desc =
			device->buffer_get_data(world_->context().render->passes().islands->desc_buffer(), 0, static_cast<uint32_t>(desc_bytes));
	if (desc.size() != desc_bytes) return d;
	const uint8_t *src = desc.ptr() + static_cast<int64_t>(island_slot) * 128;
	const float *f = reinterpret_cast<const float *>(src);
	const int32_t *di = reinterpret_cast<const int32_t *>(src);
	const int dim = di[16];
	const int volume_slot = di[17];
	if (dim < 2 || volume_slot < 0 || volume_slot >= ve::kMaxVolumes) return d;
	float basis[9];   // column major: basis[a*3+c] = world direction component c of local +a
	float body_origin[3], lattice_lo[3];
	for (int a = 0; a < 3; a++) {
		basis[a * 3 + 0] = f[a * 4 + 0];
		basis[a * 3 + 1] = f[a * 4 + 1];
		basis[a * 3 + 2] = f[a * 4 + 2];
		body_origin[a] = f[a * 4 + 3];
		lattice_lo[a] = f[12 + a];
	}
	const float voxel = f[15];
	if (!(voxel > 0.0f)) return d;
	const ve::VolumeData *vol = world_->context().store->volumes().get(volume_slot);
	if (!vol || !vol->has_normals() || vol->dim != dim) return d;

	const float p[3] = {origin.x, origin.y, origin.z};
	const float basis_f[3] = {dir.x, dir.y, dir.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, basis_f, w, h,
			1.0471975512f, 0.05f, 4000.0f);
	ve::CameraParams cam{};
	for (int axis = 0; axis < 3; ++axis) {
		cam.cam_pos[axis] = p[axis];
		cam.cam_right[axis] = pc.right[axis];
		cam.cam_up[axis] = pc.up[axis];
		cam.cam_fwd[axis] = pc.fwd[axis];
	}
	cam.params[0] = pc.tan_x;
	cam.params[1] = pc.tan_y;
	cam.params[2] = 200.0f;
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cam, ve::pack_flags(world_->context().render->beauty_settings()));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cam, w, h, kNoEdit, world_->context().render->passes().field_context)) return d;
	device->submit();
	device->sync();
	const PackedByteArray surface = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	const PackedByteArray hitpos = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (surface.size() < static_cast<int64_t>(w) * h * 8) return d;
	if (hitpos.size() < static_cast<int64_t>(w) * h * 16) return d;
	const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
	const float *hp = reinterpret_cast<const float *>(hitpos.ptr());
	constexpr float kSun[3] = {0.5746958f, 0.7662610f, 0.2873479f};
	constexpr float kEdges[3] = {0.08f, 0.32f, 0.66f};
	auto band = [](float ndl) { return ndl > 0.66f ? 3 : ndl > 0.32f ? 2 : ndl > 0.08f ? 1 : 0; };

	int total_hits = 0;
	for (int i = 0; i < w * h; i++) if (hp[i * 4 + 3] > 0.5f) total_hits++;
	std::vector<uint8_t> mismatch(static_cast<size_t>(w) * h, 0);
	double sum_sq = 0.0;
	int considered = 0;
	int mismatches = 0;
	const float span = static_cast<float>(dim - 1) * voxel;
	const int sy = dim, sz = dim * dim;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int i = y * w + x;
			if (hp[i * 4 + 3] <= 0.5f) continue;
			const Vector3 hit(hp[i * 4 + 0], hp[i * 4 + 1], hp[i * 4 + 2]);
			// World -> local -> lattice coordinates.
			const float rel[3] = {hit.x - body_origin[0], hit.y - body_origin[1],
					hit.z - body_origin[2]};
			// Orthonormal basis: inverse is its transpose.
			float q_l[3];
			for (int a = 0; a < 3; a++)
				q_l[a] = basis[a * 3 + 0] * rel[0] + basis[a * 3 + 1] * rel[1]
						+ basis[a * 3 + 2] * rel[2];
			bool inside = true;
			float lcoord[3];
			for (int a = 0; a < 3; a++) {
				lcoord[a] = (q_l[a] - lattice_lo[a]) / voxel;
				if (lcoord[a] < -1e-4f || lcoord[a] > static_cast<float>(dim - 1) + 1e-4f)
					inside = false;
			}
			if (!inside) continue; // terrain (or another body) behind/around the island
			// Trilinear blend of the compact normals at the same coords/fractions the
			// shader's island_sdf_at uses.
			int i0[3], i1[3];
			float fr[3];
			for (int a = 0; a < 3; a++) {
				lcoord[a] = std::min(std::max(lcoord[a], 0.0f), static_cast<float>(dim - 1));
				i0[a] = static_cast<int>(std::floor(lcoord[a]));
				i1[a] = std::min(i0[a] + 1, dim - 1);
				fr[a] = lcoord[a] - static_cast<float>(i0[a]);
			}
			float n_acc[3] = {0.0f, 0.0f, 0.0f};
			for (int c = 0; c < 8; c++) {
				const int cx = (c & 1) ? i1[0] : i0[0];
				const int cy = (c & 2) ? i1[1] : i0[1];
				const int cz = (c & 4) ? i1[2] : i0[2];
				float wx = (c & 1) ? fr[0] : 1.0f - fr[0];
				float wy = (c & 2) ? fr[1] : 1.0f - fr[1];
				float wz = (c & 4) ? fr[2] : 1.0f - fr[2];
				float dec[3];
				ve::oct_decode_snorm8(vol->normal_oct[static_cast<size_t>(cx + cy * sy + cz * sz)], dec);
				for (int a = 0; a < 3; a++) n_acc[a] += wx * wy * wz * dec[a];
			}
			// Local -> world through the body basis. basis[a*3+c] is the world component c
			// of local axis a, so world[c] = sum over local axes a of basis[a*3+c] * n[a]
			// (the same column convention march_island's mat3 multiply uses).
			float wn[3] = {0.0f, 0.0f, 0.0f};
			for (int a = 0; a < 3; a++) {
				const float na = n_acc[a];
				wn[0] += basis[a * 3 + 0] * na;
				wn[1] += basis[a * 3 + 1] * na;
				wn[2] += basis[a * 3 + 2] * na;
			}
			float alen = std::sqrt(wn[0]*wn[0] + wn[1]*wn[1] + wn[2]*wn[2]);
			if (alen < 1e-6f) continue; // degenerate blend: nothing to compare against
			float an[3] = {wn[0]/alen, wn[1]/alen, wn[2]/alen};
			const float e[2] = {half_to_float(s[i * 4 + 0]), half_to_float(s[i * 4 + 1])};
			float rn[3];
			ve::oct_decode(e, rn);
			float rlen = std::sqrt(rn[0]*rn[0] + rn[1]*rn[1] + rn[2]*rn[2]);
			if (rlen > 1e-8f) { rn[0]/=rlen; rn[1]/=rlen; rn[2]/=rlen; }
			float ndl_render = rn[0]*kSun[0] + rn[1]*kSun[1] + rn[2]*kSun[2];
			float ndl_analytic = an[0]*kSun[0] + an[1]*kSun[1] + an[2]*kSun[2];
			const double diff = double(ndl_render) - double(ndl_analytic);
			sum_sq += diff * diff;
			considered++;
			bool far_from_edges = true;
			for (int eidx = 0; eidx < 3; eidx++) if (std::fabs(ndl_analytic - kEdges[eidx]) < 0.01f) far_from_edges = false;
			if (!far_from_edges) continue;
			if (band(ndl_render) != band(ndl_analytic)) {
				mismatch[static_cast<size_t>(i)] = 1;
				mismatches++;
			}
		}
	}
	double rms = considered > 0 ? std::sqrt(sum_sq / double(considered)) : 0.0;
	double frac = considered > 0 ? double(mismatches) / double(considered) : 0.0;
	int largest = 0;
	std::vector<uint8_t> visited(static_cast<size_t>(w) * h, 0);
	std::vector<int> stack;
	stack.reserve(1024);
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int idx = y * w + x;
			if (!mismatch[static_cast<size_t>(idx)] || visited[static_cast<size_t>(idx)]) continue;
			int comp = 0;
			stack.clear();
			stack.push_back(idx);
			visited[static_cast<size_t>(idx)] = 1;
			while (!stack.empty()) {
				int cur = stack.back(); stack.pop_back();
				comp++;
				int cx = cur % w;
				int cy = cur / w;
				if (cx > 0) { int nb = cur - 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cx + 1 < w) { int nb = cur + 1; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy > 0) { int nb = cur - w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
				if (cy + 1 < h) { int nb = cur + w; if (mismatch[static_cast<size_t>(nb)] && !visited[static_cast<size_t>(nb)]) { visited[static_cast<size_t>(nb)] = 1; stack.push_back(nb); } }
			}
			if (comp > largest) largest = comp;
		}
	}
	d["ran"] = true;
	d["hits"] = total_hits;
	d["considered"] = considered; // hits whose CPU reference gradient was exact and compared
	d["rms_ndl"] = rms;
	d["cel_mismatch_fraction"] = frac;
	d["largest_mismatch_component"] = largest;
	return d;
}

Dictionary VoxelDebugHooks::debug_ssr_probe(int fixture, int w, int h) {
	Dictionary d;
	const int width = std::max(1, w);
	const int height = std::max(1, h);
	d["width"] = std::max(1, width / 2);
	d["height"] = std::max(1, height / 2);
	d["ran"] = false;
	d["steps"] = 0;
	d["hit_pixels"] = 0;
	d["dynamic_hit_pixels"] = 0;
	d["scene_hit_pixels"] = 0;
	d["red_gain"] = 0.0f;
	d["max_weight"] = 0.0f;
	d["max_alpha_delta"] = 0.0f;
	d["mean_delta"] = 0.0f;
	d["finite"] = true;
	if (w <= 0 || h <= 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch || !world_->context().render->passes().composite ||
			!world_->context().render->passes().gbuffer || !world_->context().render->passes().beauty_camera || !world_->context().render->passes().ssr) return d;
	const ve::BeautySettings settings = world_->context().render->beauty_settings();
	d["steps"] = settings.ssr_steps;
	if (!settings.ssr || settings.ssr_steps <= 0) return d;
	const Vector2i size(width, height);
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, size) || !world_->context().render->passes().beauty_camera->ensure(device)) return d;
	const float camera_pos[3] = {20.0f, 75.0f, 20.0f};
	const float camera_fwd[3] = {0.0f, -1.0f, 0.0f};
	const ve::ProbeCamera pc = ve::probe_camera(camera_pos, camera_fwd, width, height,
			1.0471975512f, 0.05f, 4000.0f);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = pc.lod.view_proj[c * 4 + r];
	float normal[3] = {0.6f, 0.8f, 0.0f};
	float oct[2];
	ve::oct_encode(normal, oct);
	ve::CameraParams camera_params{};
	for (int axis = 0; axis < 3; ++axis) {
		camera_params.cam_pos[axis] = camera_pos[axis];
		camera_params.cam_right[axis] = pc.right[axis];
		camera_params.cam_up[axis] = pc.up[axis];
		camera_params.cam_fwd[axis] = pc.fwd[axis];
	}
	camera_params.params[0] = pc.tan_x;
	camera_params.params[1] = pc.tan_y;
	camera_params.params[2] = 200.0f;
	ve::set_near_field_world(&camera_params, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&camera_params, ve::pack_flags(settings));
	static const float no_edit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), camera_params, width, height,
			no_edit, world_->context().render->passes().field_context)) return d;
	float fade_start = ve::kLodFadeStartM, fade_end = ve::kLodFadeEndM;
	world_->context().lod->fade_band(&fade_start, &fade_end);
	world_->context().render->passes().composite->draw(device, *world_->context().render->passes().gbuffer, world_->context().render->passes().raymarch->albedo_texture(),
			world_->context().render->passes().raymarch->surface_texture(), world_->context().render->passes().raymarch->hitpos_texture(), view_proj,
			*world_->context().render->passes().materials, camera_params, fade_start, fade_end);
	if (!world_->context().render->passes().composite->last_draw_ok()) return d;
	auto float16 = [](float value) -> uint16_t {
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		const uint32_t sign = (bits >> 16) & 0x8000u;
		const int exp = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
		const uint32_t mant = bits & 0x7FFFFFu;
		if (exp <= 0) return static_cast<uint16_t>(sign);
		if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
		return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
				(mant >> 13));
	};
	const int pixels = width * height;
	PackedByteArray depths;
	depths.resize(pixels * static_cast<int>(sizeof(float)));
	float *depth_ptr = reinterpret_cast<float *>(depths.ptrw());
	PackedByteArray colors;
	colors.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *color_ptr = reinterpret_cast<uint16_t *>(colors.ptrw());
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			const int index = y * width + x;
			const bool blocker = (fixture == 0 || fixture >= 2) && x >= width / 4 && x < width / 2 &&
					y >= height / 4 && y < height / 2;
			depth_ptr[index] = 0.0f;
			const float r = blocker ? 1.0f : 0.2f;
			const float g = blocker ? 0.03f : 0.2f;
			const float b = blocker ? 0.03f : 0.2f;
			color_ptr[index * 4 + 0] = float16(r);
			color_ptr[index * 4 + 1] = float16(g);
			color_ptr[index * 4 + 2] = float16(b);
			color_ptr[index * 4 + 3] = float16(0.7f);
		}
	}
	PackedByteArray surface_bytes;
	surface_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *surface_ptr = reinterpret_cast<uint16_t *>(surface_bytes.ptrw());
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			const int i = y * width + x;
			const bool dynamic_receiver = fixture >= 2 && x < width / 3 && y < height / 3;
			surface_ptr[i * 4 + 0] = dynamic_receiver ? float16(0.0f) : float16(oct[0]);
			surface_ptr[i * 4 + 1] = dynamic_receiver ? float16(0.0f) : float16(oct[1]);
			surface_ptr[i * 4 + 2] = dynamic_receiver ? float16(0.0f) : float16(1.0f);
			surface_ptr[i * 4 + 3] = dynamic_receiver ? float16(0.0f) : float16(1.0f);
		}
	Ref<RDTextureFormat> depth_format;
	depth_format.instantiate();
	depth_format->set_format(RenderingDevice::DATA_FORMAT_R32_SFLOAT);
	depth_format->set_width(width); depth_format->set_height(height);
	depth_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> depth_view; depth_view.instantiate();
	TypedArray<PackedByteArray> depth_data; depth_data.push_back(depths);
	const RID scene_depth = device->texture_create(depth_format, depth_view, depth_data);
	Ref<RDTextureFormat> color_format;
	color_format.instantiate();
	color_format->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
	color_format->set_width(width); color_format->set_height(height);
	color_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> color_view; color_view.instantiate();
	TypedArray<PackedByteArray> color_data; color_data.push_back(colors);
	const RID scene_color = device->texture_create(color_format, color_view, color_data);
	Ref<RDTextureFormat> surface_format;
	surface_format.instantiate();
	surface_format->set_format(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT);
	surface_format->set_width(width); surface_format->set_height(height);
	surface_format->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> surface_view; surface_view.instantiate();
	TypedArray<PackedByteArray> surface_data; surface_data.push_back(surface_bytes);
	const RID fixture_surface = device->texture_create(surface_format, surface_view, surface_data);
	RID normal_texture;
	if (fixture >= 2) {
		PackedByteArray normal_bytes;
		normal_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
		uint16_t *normal_ptr = reinterpret_cast<uint16_t *>(normal_bytes.ptrw());
		const uint16_t alpha = float16(fixture == 2 ? 0.0f : 1.0f);
		for (int i = 0; i < pixels; i++) {
			normal_ptr[i * 4 + 0] = float16(0.0f);
			normal_ptr[i * 4 + 1] = float16(0.0f);
			normal_ptr[i * 4 + 2] = float16(0.0f);
			normal_ptr[i * 4 + 3] = alpha;
		}
		TypedArray<PackedByteArray> normal_data;
		normal_data.push_back(normal_bytes);
		normal_texture = device->texture_create(color_format, color_view, normal_data);
	}
	if (!scene_depth.is_valid() || !scene_color.is_valid() || !fixture_surface.is_valid() ||
			(fixture >= 2 && !normal_texture.is_valid())) {
		if (scene_depth.is_valid()) device->free_rid(scene_depth);
		if (scene_color.is_valid()) device->free_rid(scene_color);
		if (fixture_surface.is_valid()) device->free_rid(fixture_surface);
		if (normal_texture.is_valid()) device->free_rid(normal_texture);
		return d;
	}
	device->submit();
	device->sync();
	const PackedByteArray rendered_depth = device->texture_get_data(world_->context().render->passes().gbuffer->depth(), 0);
	if (rendered_depth.size() < pixels * static_cast<int>(sizeof(float))) {
		device->free_rid(scene_depth);
		device->free_rid(scene_color);
		device->free_rid(fixture_surface);
		if (normal_texture.is_valid()) device->free_rid(normal_texture);
		return d;
	}
	const float *rendered_depth_ptr = reinterpret_cast<const float *>(rendered_depth.ptr());
	const Projection inv_view_proj = view_proj.inverse();
	auto mul_projection = [](const Projection &m, const Vector4 &v) {
		Vector4 result;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) result[r] += m.columns[c][r] * v[c];
		return result;
	};
	auto dynamic_plane_depth = [&](int x, int y) {
		const Vector2 ndc((static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f,
				(static_cast<float>(y) + 0.5f) / height * 2.0f - 1.0f);
		const Vector4 far_h = mul_projection(inv_view_proj, Vector4(ndc.x, ndc.y, 0.0f, 1.0f));
		const Vector3 far_p = Vector3(far_h.x, far_h.y, far_h.z) / far_h.w;
		const Vector3 ray = (far_p - Vector3(camera_pos[0], camera_pos[1], camera_pos[2])).normalized();
		const Vector3 plane_normal(0.6f, 0.8f, 0.0f);
		const Vector3 plane_point(20.0f, 60.0f, 20.0f);
		const float denom = plane_normal.dot(ray);
		if (std::fabs(denom) < 1e-5f) return rendered_depth_ptr[y * width + x];
		const Vector3 hit = Vector3(camera_pos[0], camera_pos[1], camera_pos[2]) + ray *
				(plane_normal.dot(plane_point - Vector3(camera_pos[0], camera_pos[1], camera_pos[2])) / denom);
		const Vector4 clip = mul_projection(view_proj, Vector4(hit.x, hit.y, hit.z, 1.0f));
		return clip.z / clip.w;
	};
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			const int index = y * width + x;
			const bool dynamic_receiver = fixture >= 2 && x < width / 3 && y < height / 3;
			const bool blocker = (fixture == 0 || fixture >= 2) && x >= width / 4 && x < width / 2 &&
					y >= height / 4 && y < height / 2;
			const float base_depth = dynamic_receiver ? dynamic_plane_depth(x, y) : rendered_depth_ptr[index];
			depth_ptr[index] = blocker ? std::min(1.0f, base_depth + 0.001f) : base_depth;
		}
	device->texture_update(scene_depth, 0, depths);
	device->texture_update(scene_color, 0, colors);
	device->texture_copy(fixture_surface, world_->context().render->passes().gbuffer->surface(), Vector3(), Vector3(),
			Vector3(width, height, 1), 0, 0, 0, 0);
	device->submit();
	device->sync();
	world_->context().render->passes().beauty_camera->update(device, view_proj, camera_pos, size, 0.05f, 4000.0f);
	const RID normal_roughness = normal_texture;
	const bool ok = world_->context().render->passes().ssr->render(device, scene_color, scene_depth, world_->context().render->passes().gbuffer->surface(),
			world_->context().render->passes().gbuffer->depth(), normal_roughness, normal_roughness.is_valid(), world_->context().render->passes().beauty_camera->buffer(),
			size, settings);
	device->submit();
	device->sync();
	auto release_fixture = [&]() {
		world_->context().render->passes().ssr->teardown();
		if (!world_->context().render->passes().ssr->initialize(device))
			UtilityFunctions::printerr("debug_ssr_probe: SSR pass reinitialization failed");
		device->free_rid(scene_depth);
		device->free_rid(scene_color);
		device->free_rid(fixture_surface);
		if (normal_texture.is_valid()) device->free_rid(normal_texture);
	};
	if (!ok) {
		release_fixture();
		return d;
	}
	const PackedByteArray before = colors;
	const PackedByteArray after = device->texture_get_data(scene_color, 0);
	const PackedByteArray reflection = device->texture_get_data(world_->context().render->passes().ssr->reflection(), 0);
	const int half_w = std::max(1, width / 2), half_h = std::max(1, height / 2);
	const int half_pixels = half_w * half_h;
	if (after.size() >= pixels * 8 && reflection.size() >= half_pixels * 8) {
		const uint16_t *a = reinterpret_cast<const uint16_t *>(after.ptr());
		const uint16_t *r = reinterpret_cast<const uint16_t *>(reflection.ptr());
		const uint16_t *b = reinterpret_cast<const uint16_t *>(before.ptr());
		double delta = 0.0;
		float red_gain_max = 0.0f;
		float max_weight = 0.0f, max_alpha_delta = 0.0f;
		int hits = 0, dynamic_hits = 0, scene_hits = 0;
		for (int i = 0; i < pixels; i++) {
			const float br = Math::half_to_float(b[i * 4]);
			const float ar = Math::half_to_float(a[i * 4]);
			const float ba = Math::half_to_float(b[i * 4 + 3]);
			const float aa = Math::half_to_float(a[i * 4 + 3]);
			red_gain_max = std::max(red_gain_max, ar - br);
			delta += std::fabs(ar - br) + std::fabs(Math::half_to_float(a[i * 4 + 1]) -
					Math::half_to_float(b[i * 4 + 1]));
			max_alpha_delta = std::max(max_alpha_delta, std::fabs(aa - ba));
		}
		for (int i = 0; i < half_pixels; i++) {
			const float weight = Math::half_to_float(r[i * 4 + 3]);
			max_weight = std::max(max_weight, weight);
			if (weight > 0.001f) {
				hits++;
				const int sx = (i % half_w) * width / half_w;
				const int sy = (i / half_w) * height / half_h;
				if (fixture >= 2 && sx < width / 3 && sy < height / 3)
					dynamic_hits++;
				else
					scene_hits++;
			}
		}
		d["hit_pixels"] = hits;
		d["dynamic_hit_pixels"] = dynamic_hits;
		d["scene_hit_pixels"] = scene_hits;
		d["red_gain"] = red_gain_max;
		d["mean_delta"] = static_cast<float>(delta / pixels);
		d["max_weight"] = max_weight;
		d["max_alpha_delta"] = max_alpha_delta;
		for (int i = 0; i < pixels * 4; i++)
			if (!std::isfinite(Math::half_to_float(a[i]))) d["finite"] = false;
	}
	d["ran"] = true;
	release_fixture();
	return d;
}

Dictionary VoxelDebugHooks::debug_outline_probe(int fixture, bool have_dynamic_normals) {
	Dictionary d;
	d["ran"] = false;
	d["dark_columns"] = 0;
	d["dark_value"] = 0.0f;
	d["mean_delta"] = 0.0f;
	d["max_brightening"] = 0.0f;
	d["max_alpha_delta"] = 0.0f;
	if (fixture < 0) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().outline || !world_->context().render->passes().beauty_camera) return d;
	const int width = 32, height = 16, pixels = width * height;
	if (!world_->context().render->passes().beauty_camera->ensure(device)) return d;
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = c == r ? 1.0f : 0.0f;
	const float camera_pos[3] = {0.0f, 0.0f, -100.0f};
	world_->context().render->passes().beauty_camera->update(device, view_proj, camera_pos, Vector2i(width, height), 0.05f,
			4000.0f);

	auto float16 = [](float value) -> uint16_t {
		uint32_t bits;
		std::memcpy(&bits, &value, sizeof(bits));
		const uint32_t sign = (bits >> 16) & 0x8000u;
		const int exp = static_cast<int>((bits >> 23) & 0xFFu) - 127 + 15;
		const uint32_t mant = bits & 0x7FFFFFu;
		if (exp <= 0) return static_cast<uint16_t>(sign);
		if (exp >= 31) return static_cast<uint16_t>(sign | 0x7C00u);
		return static_cast<uint16_t>(sign | (static_cast<uint32_t>(exp) << 10) |
				(mant >> 13));
	};
	const uint16_t one = float16(1.0f), half = float16(0.5f);
	PackedByteArray depth_bytes;
	depth_bytes.resize(pixels * static_cast<int>(sizeof(float)));
	float *depth = reinterpret_cast<float *>(depth_bytes.ptrw());
	PackedByteArray color_bytes;
	color_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *color = reinterpret_cast<uint16_t *>(color_bytes.ptrw());
	PackedByteArray gb_depth_bytes;
	gb_depth_bytes.resize(pixels * static_cast<int>(sizeof(float)));
	float *gb_depth = reinterpret_cast<float *>(gb_depth_bytes.ptrw());
	PackedByteArray surface_bytes;
	surface_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
	uint16_t *surface = reinterpret_cast<uint16_t *>(surface_bytes.ptrw());
	PackedByteArray normal_bytes;
	uint16_t *normal = nullptr;
	if (fixture == 3) {
		normal_bytes.resize(pixels * 4 * static_cast<int>(sizeof(uint16_t)));
		normal = reinterpret_cast<uint16_t *>(normal_bytes.ptrw());
	}
	// Fixtures 5-7 exercise the two ways the edge test used to report an edge where the
	// geometry has none. All three cover the whole frame with terrain (material != 0).
	//
	//   5  seam hole  one column lost its depth to the near/far dither while its g-buffer
	//                 surface survived. Not a silhouette; nothing may darken.
	//   6  real sky   everything from that column rightwards is background, material 0 and
	//                 all. That IS a silhouette, and its last terrain column must darken.
	//   7  grazing    a depth ramp seen almost edge-on, holding the relative depth step at
	//                 a constant 6% -- over the old flat 4% threshold, under the incidence-
	//                 scaled one -- with a genuine cliff at column 24 that must still show.
	//   8  chunk-face crack  ONE column with neither depth nor material, with the same
	//                 surface at a continuous depth on both sides. That is the hairline hole
	//                 the far field's raster leaves where two LoD levels meet on a chunk
	//                 face, and it is a rasterisation gap, not a silhouette.
	const bool hole_fixture = fixture == 5 || fixture == 8;
	const bool sky_fixture = fixture == 6;
	const int hole_column = width / 2;
	const int cliff_column = 24;
	const float kGrazingStep = 0.06f;   // relative depth step per column
	// The cliff has to clear the incidence-scaled threshold, which at this fixture's
	// near-edge-on angle is the OUTLINE_MIN_NDV cap: 0.04 / 0.05 = 0.8 relative.
	const float kCliffFactor = 20.0f;
	std::vector<float> ramp(width, 10.0f), ramp_ndv(width, 1.0f);
	if (fixture == 7) {
		// The camera sits at z = -100 and the identity view_proj puts the reconstructed
		// world point at (u, v, depth), so the camera distance is depth + 100.
		for (int x = 1; x < width; x++) {
			const float step = (ramp[x - 1] + 100.0f) * kGrazingStep;
			ramp[x] = ramp[x - 1] + step * (x == cliff_column ? kCliffFactor : 1.0f);
		}
		// World x spans [-1, 1] across the row, so one column advances 2/width in x while
		// the surface advances `step` along the view axis. The normal is perpendicular to
		// that tangent, which is what makes the surface edge-on.
		for (int x = 0; x < width; x++) {
			const float dz = x + 1 < width ? ramp[x + 1] - ramp[x]
					: ramp[x] - ramp[x - 1];
			const float dx = 2.0f / static_cast<float>(width);
			const float len = std::sqrt(dz * dz + dx * dx);
			ramp_ndv[x] = len > 0.0f ? dx / len : 1.0f;
		}
	}
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++) {
			const int p = y * width + x;
			const bool right_side = x >= width / 2;
			const bool depth_line = fixture == 1 || fixture == 4;
			depth[p] = depth_line && right_side ? 20.0f : 10.0f;
			if (fixture == 7) depth[p] = ramp[x];
			if (hole_fixture && x == hole_column) depth[p] = 0.0f;
			if (sky_fixture && x >= hole_column) depth[p] = 0.0f;
			gb_depth[p] = depth[p];
			color[p * 4 + 0] = one; color[p * 4 + 1] = one;
			color[p * 4 + 2] = one; color[p * 4 + 3] = one;
			const bool terrain = fixture == 2;
			// Fixture 5 keeps its surface through the hole -- that is the whole point of it.
			// Fixtures 6 and 8 drop the material too; they differ only in how WIDE the gap is,
			// which is the one thing that tells sky apart from a raster hole.
			const bool covered = terrain || fixture == 5 || fixture == 7 ||
					(sky_fixture && x < hole_column) ||
					(fixture == 8 && x != hole_column);
			const bool up = !right_side;
			surface[p * 4 + 0] = up ? half : one;
			surface[p * 4 + 1] = up ? one : half;
			if (fixture == 5 || fixture == 6 || fixture == 7 || fixture == 8) {
				// A real encoded normal, so the shader's oct_decode returns the vector the
				// incidence term needs rather than an arbitrary pair of fp16 constants. The
				// camera looks down +z here, so an incidence cosine of c is a normal whose
				// z component is -c.
				const float c = ramp_ndv[x];
				const float n[3] = {std::sqrt(std::max(0.0f, 1.0f - c * c)), 0.0f, -c};
				float e[2];
				ve::oct_encode(n, e);
				surface[p * 4 + 0] = float16(e[0]);
				surface[p * 4 + 1] = float16(e[1]);
			}
			surface[p * 4 + 2] = covered ? one : 0;
			surface[p * 4 + 3] = one;
			if (normal) {
				normal[p * 4 + 0] = up ? half : one;
				normal[p * 4 + 1] = up ? one : half;
				normal[p * 4 + 2] = half;
				normal[p * 4 + 3] = one;
			}
		}

	auto create = [&](RenderingDevice::DataFormat format, uint32_t usage,
			const PackedByteArray &bytes) -> RID {
			Ref<RDTextureFormat> f;
			f.instantiate(); f->set_format(format); f->set_width(width); f->set_height(height);
			f->set_usage_bits(usage);
			Ref<RDTextureView> v; v.instantiate();
			TypedArray<PackedByteArray> data; data.push_back(bytes);
			return device->texture_create(f, v, data);
		};
	const uint32_t sampled = RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT;
	const uint32_t color_usage = sampled | RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT;
	const RID scene_depth = create(RenderingDevice::DATA_FORMAT_R32_SFLOAT, sampled,
				depth_bytes);
		const RID scene_color = create(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
				color_usage, color_bytes);
		const RID fixture_gb_depth = create(RenderingDevice::DATA_FORMAT_R32_SFLOAT, sampled,
				gb_depth_bytes);
		const RID fixture_surface = create(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT,
				sampled, surface_bytes);
		RID normal_texture;
		if (normal)
			normal_texture = create(RenderingDevice::DATA_FORMAT_R16G16B16A16_SFLOAT, sampled,
					normal_bytes);
		const bool valid = scene_depth.is_valid() && scene_color.is_valid() &&
			fixture_gb_depth.is_valid() && fixture_surface.is_valid() &&
			(!normal || normal_texture.is_valid());
		if (!valid) {
			for (RID r : {scene_depth, scene_color, fixture_gb_depth, fixture_surface, normal_texture})
				if (r.is_valid()) device->free_rid(r);
			return d;
		}
		const ve::BeautySettings settings = world_->context().render->beauty_settings();
		const bool ok = world_->context().render->passes().outline->render(device, scene_color, scene_depth, fixture_gb_depth,
				fixture_surface, normal_texture, have_dynamic_normals && normal_texture.is_valid(),
				world_->context().render->passes().beauty_camera->buffer(), Vector2i(width, height), settings);
		device->submit();
		device->sync();
		d["ran"] = ok;
		const PackedByteArray after = device->texture_get_data(scene_color, 0);
		if (ok && after.size() >= pixels * 8) {
			const uint16_t *out = reinterpret_cast<const uint16_t *>(after.ptr());
			int dark_columns = 0;
			float dark_value = 0.0f, max_brightening = 0.0f, max_alpha_delta = 0.0f;
			double total_delta = 0.0;
			for (int x = 0; x < width; x++) {
				bool dark = false;
				for (int y = 0; y < height; y++) {
					const int p = (y * width + x) * 4;
					const float r = Math::half_to_float(out[p]);
					const float a = Math::half_to_float(out[p + 3]);
					if (r < 0.99f) { dark = true; dark_value = r; }
					max_brightening = std::max(max_brightening, r - 1.0f);
					max_alpha_delta = std::max(max_alpha_delta, std::fabs(a - 1.0f));
					total_delta += std::fabs(r - 1.0f);
				}
				if (dark) dark_columns++;
			}
			d["dark_columns"] = dark_columns;
			d["dark_value"] = dark_value;
			d["mean_delta"] = static_cast<float>(total_delta / pixels);
			d["max_brightening"] = max_brightening;
			d["max_alpha_delta"] = max_alpha_delta;
		}
		world_->context().render->passes().outline->teardown();
		world_->context().render->passes().outline->initialize(device);
		for (RID r : {scene_depth, scene_color, fixture_gb_depth, fixture_surface, normal_texture})
			if (r.is_valid()) device->free_rid(r);
		return d;
}

Dictionary VoxelDebugHooks::debug_glossy_sdf_probe(Vector3 origin, Vector3 dir) {
	Dictionary d;
	d["hit"] = false;
	d["albedo"] = Color();
	d["sun"] = 0.0f;
	d["material"] = 0;
	d["gloss"] = 0.0f;
	d["position"] = origin;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch) return d;
	const Vector3 forward = dir.normalized();
	const float f[3] = {forward.x, forward.y, forward.z};
	float up[3];
	ve::probe_up_hint(f, up);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			origin.x, origin.y, origin.z, f[0], f[1], f[2], up[0], up[1], up[2]);
	cam.params[0] = 0.0f;
	cam.params[1] = 0.0f;
	cam.params[2] = 200.0f;
	cam.params[3] = -1.0f;
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cam, ve::pack_flags(world_->context().render->beauty_settings()));
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cam, 1, 1, kNoEdit, world_->context().render->passes().field_context)) return d;
	device->submit();
	device->sync();
	const PackedByteArray ab = device->texture_get_data(world_->context().render->passes().raymarch->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	const PackedByteArray hp = device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
	if (ab.size() < 4 || sf.size() < 8 || hp.size() < 16) return d;
	const uint8_t *a = ab.ptr();
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	const float *h = reinterpret_cast<const float *>(hp.ptr());
	d["sun"] = a[3] / 255.0f;
	const int gmat = static_cast<int>(half_to_float(s[2]) + 0.5f);
	d["material"] = gmat;
	d["hit"] = h[3] > 0.5f;
	// The reflection is the point of this probe and it lives in the overlay: the marcher mixed
	// it there at the fresnel weight, and the composite mixes the pair over the material.
	const Color goverlay(a[0] / 255.0f, a[1] / 255.0f, a[2] / 255.0f, 1.0f);
	d["overlay"] = goverlay;
	d["overlay_weight"] = half_to_float(s[3]);
	float ggloss = 0.0f;
	const float e[2] = {half_to_float(s[0]), half_to_float(s[1])};
	float gn[3];
	ve::oct_decode(e, gn);
	d["albedo"] = resolve_near_field(gmat, Vector3(h[0], h[1], h[2]), Vector3(gn[0], gn[1], gn[2]),
			goverlay, half_to_float(s[3]), &ggloss);
	d["gloss"] = ggloss;
	d["position"] = Vector3(h[0], h[1], h[2]);
	return d;
}

Color VoxelDebugHooks::debug_cel_reference(Color albedo, Color ambient, float ndl, float ndv,
		float ndh, float shadow, float ao, float gloss) const {
	ve::CelParams p;
	ve::CelInput in;
	in.albedo[0] = albedo.r; in.albedo[1] = albedo.g; in.albedo[2] = albedo.b;
	in.ambient[0] = ambient.r; in.ambient[1] = ambient.g; in.ambient[2] = ambient.b;
	in.ndl = ndl; in.ndv = ndv; in.ndh = ndh;
	in.shadow = shadow; in.ao = ao; in.gloss = gloss;
	float out[3];
	ve::cel_shade(p, in, out);
	return Color(out[0], out[1], out[2], 1.0f);
}

Dictionary VoxelDebugHooks::debug_cel_diff(Color albedo, Color ambient, float ndl, float ndv,
		float ndh, float shadow, float ao, float gloss) {
	Dictionary d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer || !world_->context().render->passes().deferred || !world_->context().render->passes().materials) return d;
	if (world_->context().render->passes().gbuffer->size() != Vector2i(1, 1)) {
		world_->context().render->passes().deferred->teardown();
		world_->context().render->passes().deferred->initialize(device);
		if (world_->context().render->passes().composite) world_->context().render->passes().composite->release_targets();
	}
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(1, 1))) return d;
	DeferredPass::Params p;
	p.probe_mode = 1;
	p.inv_view_proj[0] = albedo.r;
	p.inv_view_proj[1] = albedo.g;
	p.inv_view_proj[2] = albedo.b;
	p.inv_view_proj[4] = ambient.r;
	p.inv_view_proj[5] = ambient.g;
	p.inv_view_proj[6] = ambient.b;
	p.inv_view_proj[8] = ndl;
	p.inv_view_proj[9] = ndv;
	p.inv_view_proj[10] = ndh;
	p.inv_view_proj[11] = shadow;
	p.inv_view_proj[12] = ao;
	p.inv_view_proj[13] = gloss;
	if (!world_->context().render->passes().deferred->render(device, *world_->context().render->passes().gbuffer, *world_->context().render->passes().materials, RID(), RID(), RID(), p))
		return d;
	device->submit();
	device->sync();
	const PackedByteArray got = device->texture_get_data(world_->context().render->passes().gbuffer->lit(), 0);
	if (got.size() < 8) return d;
	const uint16_t *h = reinterpret_cast<const uint16_t *>(got.ptr());
	const Color gpu(half_to_float(h[0]), half_to_float(h[1]), half_to_float(h[2]), 1.0f);
	ve::CelParams params;
	ve::CelInput in;
	in.albedo[0] = albedo.r; in.albedo[1] = albedo.g; in.albedo[2] = albedo.b;
	in.ambient[0] = ambient.r; in.ambient[1] = ambient.g; in.ambient[2] = ambient.b;
	in.ndl = ndl; in.ndv = ndv; in.ndh = ndh;
	in.shadow = shadow; in.ao = ao; in.gloss = gloss;
	float ref[3];
	ve::cel_shade(params, in, ref);
	const Color cpu(ref[0], ref[1], ref[2], 1.0f);
	d["gpu"] = gpu;
	d["cpu"] = cpu;
	d["max_delta"] = std::max({std::fabs(gpu.r - cpu.r), std::fabs(gpu.g - cpu.g),
			std::fabs(gpu.b - cpu.b)});
	return d;
}

Dictionary VoxelDebugHooks::debug_sun_shadow_stats(int cascade) {
	Dictionary d;
	d["size"] = SunShadowPass::kSize;
	d["cascades"] = 0;
	d["cascade"] = cascade;
	d["radius"] = 0.0f;
	d["min_level"] = 0;
	d["map_valid"] = false;
	d["ortho_valid"] = false;
	d["texel_world"] = 0.0f;
	d["rebuilds"] = 0;
	d["pages"] = 0;
	d["view_proj"] = PackedFloat32Array();
	world_->ensure_initialized();
	SunShadowPass *sun = world_->context().render->passes().sun_shadow;
	if (!sun) return d;
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(world_->get_stream_radius_m(), SunShadowPass::kSize, c);
	d["cascades"] = n;
	if (cascade < 0 || cascade >= n) return d;
	d["radius"] = c[cascade].radius;
	d["min_level"] = c[cascade].min_level;
	// The SHIPPING fit, not a second one that happens to agree.
	const ve::SunOrtho ortho = world_->context().render->frame().sun_ortho(cascade);
	d["map_valid"] = sun->map().is_valid();
	d["ortho_valid"] = ortho.valid;
	d["texel_world"] = ortho.valid ? ortho.texel_world : sun->texel_world(cascade);
	d["rebuilds"] = sun->rebuilds(cascade);
	d["pages"] = sun->last_pages(cascade);
	// Exposed so a test can assert the query and the build agree. They share
	// should_rebuild(), so agreement is structural -- but a cut skipped on a false negative
	// is a shadow that silently stops updating, which is worth pinning. This is the LIVE
	// poll: each read advances the throttle exactly as the compositor's per-frame poll
	// does. rebuild_pending below is the non-advancing snapshot for display.
	d["needs_rebuild"] = sun->needs_rebuild(cascade, ortho);
	d["rebuild_pending"] = sun->rebuild_pending(cascade, ortho);
	PackedFloat32Array matrix;
	matrix.resize(16);
	const float *source = sun->rebuilds(cascade) > 0 ? sun->view_proj(cascade) :
			(ortho.valid ? ortho.view_proj : sun->view_proj(cascade));
	for (int i = 0; i < 16; i++) matrix.set(i, source[i]);
	d["view_proj"] = matrix;
	return d;
}

bool VoxelDebugHooks::debug_sun_shadow_build(int cascade, bool force) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().sun_shadow || !world_->context().lod->pool() || !world_->context().render->passes().lod_raster) return false;
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(world_->get_stream_radius_m(), SunShadowPass::kSize, c);
	if (cascade < 0 || cascade >= n) return false;
	// The shipping fit; see debug_sun_shadow_stats() above for why this must not be a
	// second, locally reasonable one.
	const ve::SunOrtho ortho = world_->context().render->frame().sun_ortho(cascade);
	// The compositor's poll-then-build, mirrored: the poll advances the per-cascade
	// throttle, and a decline skips the cut (for cascade 2 the expensive half) exactly
	// as the game path does -- so an unforced hook build behaves like one compositor
	// frame, and forced builds skip the poll. The raster is restored on both paths.
	if (!force && !world_->context().render->passes().sun_shadow->needs_rebuild(cascade, ortho)) {
		world_->context().lod->prepare_raster();
		return false;
	}
	// The cascade's cut, at the clamp the world is configured with -- the same cut the
	// compositor would produce, so the knob measures through this hook too.
	world_->context().lod->prepare_shadow_raster(c[cascade].radius,
			world_->get_sun_cascade_min_level() ? c[cascade].min_level : 0);
	const bool did = world_->context().render->passes().sun_shadow->build(device, *world_->context().lod->pool(), *world_->context().render->passes().lod_raster,
			cascade, ortho, force);
	world_->context().lod->prepare_raster();
	return did;
}

float VoxelDebugHooks::sun_shadow_probe(Vector3 p, Vector3 viewer, int probe_mode) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!device || !world_->context().render->passes().gbuffer || !world_->context().render->passes().deferred || !world_->context().render->passes().materials) return 1.0f;
	if (world_->context().render->passes().gbuffer->size() != Vector2i(1, 1)) {
		world_->context().render->passes().deferred->teardown();
		world_->context().render->passes().deferred->initialize(device);
		if (world_->context().render->passes().composite) world_->context().render->passes().composite->release_targets();
	}
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(1, 1))) return 1.0f;
	const ve::BeautySettings beauty = world_->context().render->beauty_settings();
	SunShadowPass *sun = world_->context().render->passes().sun_shadow;
	ve::SunCascade cascades[ve::kSunCascades];
	const int cascade_count = ve::sun_cascades(world_->get_stream_radius_m(),
			SunShadowPass::kSize, cascades);
	const bool use_sun = sun && sun->is_valid() &&
			sun->rebuilds(0) > 0 && beauty.sun_shadow_map;
	DeferredPass::Params dp;
	dp.cam_pos[0] = p.x;
	dp.cam_pos[1] = p.y;
	dp.cam_pos[2] = p.z;
	dp.flags = ve::pack_flags(beauty);
	for (int k = 0; k < 3; k++) dp.ambient[k] = beauty.ambient[k];
	dp.cascade_count = use_sun ? cascade_count : 0;
	for (int i = 0; i < cascade_count && use_sun; i++) {
		const float *vp = sun->view_proj(i);
		for (int k = 0; k < 16; k++) dp.sun_view_proj[i][k] = vp[k];
		dp.shadow_texel[i] = sun->texel_world(i);
		dp.shadow_depth_range_c[i] = sun->depth_range(i);
		dp.cascade_split[i] = cascades[i].radius;
	}
	world_->context().lod->fade_band(&dp.fade_start, &dp.fade_end);
	dp.probe_mode = probe_mode;
	// Mode 4 reads the viewer out of inv_view_proj's first row; mode 3 ignores it. Probe 3
	// asks what the map says at p, and the map is per-cascade: the cascade is selected by
	// viewing distance, so the viewer must be a real camera position, not p itself (which
	// would pin the distance at 0 and always read cascade 0). The last walk's camera is the
	// position the fits were centred on; without one yet, fall back to p (old behaviour).
	float viewer_pos[3] = { viewer.x, viewer.y, viewer.z };
	if (probe_mode == 3) {
		float cam[3];
		if (world_->context().lod->last_camera(cam)) {
			viewer_pos[0] = cam[0];
			viewer_pos[1] = cam[1];
			viewer_pos[2] = cam[2];
		}
	}
	dp.inv_view_proj[0] = viewer_pos[0];
	dp.inv_view_proj[1] = viewer_pos[1];
	dp.inv_view_proj[2] = viewer_pos[2];
	if (!world_->context().render->passes().deferred->render(device, *world_->context().render->passes().gbuffer, *world_->context().render->passes().materials, RID(), RID(),
			use_sun ? sun->map() : RID(), dp))
		return 1.0f;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(world_->context().render->passes().gbuffer->lit(), 0);
	if (data.size() < 8) return 1.0f;
	const uint16_t *value = reinterpret_cast<const uint16_t *>(data.ptr());
	return half_to_float(value[0]);
}

float VoxelDebugHooks::debug_sun_shadow_visibility(Vector3 p) {
	return sun_shadow_probe(p, p, 3);
}

float VoxelDebugHooks::debug_sun_shadow_shading(Vector3 p, Vector3 viewer) {
	return sun_shadow_probe(p, viewer, 4);
}

Dictionary VoxelDebugHooks::debug_deferred_probe(Vector3 pos, Vector3 fwd, int w, int h,
		int probe_mode) {
	Dictionary d;
	if (w <= 0 || h <= 0 ||
			(probe_mode != 0 && probe_mode != 1 && probe_mode != 2 && probe_mode != 5)) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer) return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++) {
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;
	}
	// The shipped frame, headless, with the deferred pass's debug view selected. Cascades, the
	// fade band, SSGI and SSAO are whatever the frame gave the deferred pass.
	FrameInputs in = VoxelFrame::looking_at(pos, fwd, w, h);
	in.debug.deferred_view = probe_mode;
	world_->context().render->frame().render_headless(device, in);
	device->submit();
	device->sync();
	if (!world_->context().render->frame().last_frame().stage_ok(kStageDeferred)) return d;
	const Projection view_proj = in.proj * Projection(in.cam.affine_inverse());
	const Projection inv = view_proj.inverse();
	const PackedByteArray data = device->texture_get_data(world_->context().render->passes().gbuffer->lit(), 0);
	const int pixels = w * h;
	if (data.size() < static_cast<int64_t>(pixels) * 8) return d;
	const uint16_t *values = reinterpret_cast<const uint16_t *>(data.ptr());
	const int center = (h / 2) * w + (w / 2);
	if (probe_mode == 2) {
		float center_sum[3] = {};
		for (int oy = -1; oy <= 0; oy++)
			for (int ox = -1; ox <= 0; ox++) {
				const int sample = (std::max(0, h / 2 + oy) * w) + std::max(0, w / 2 + ox);
				center_sum[0] += half_to_float(values[sample * 4]);
				center_sum[1] += half_to_float(values[sample * 4 + 1]);
				center_sum[2] += half_to_float(values[sample * 4 + 2]);
			}
		d["center"] = Vector3(center_sum[0] * 0.25f, center_sum[1] * 0.25f, center_sum[2] * 0.25f);
	} else {
		d["center"] = Color(half_to_float(values[center * 4]), half_to_float(values[center * 4 + 1]),
				half_to_float(values[center * 4 + 2]), 1.0f);
	}
	double mean_luma = 0.0;
	for (int i = 0; i < pixels; i++) {
		const float r = half_to_float(values[i * 4]);
		const float g = half_to_float(values[i * 4 + 1]);
		const float b = half_to_float(values[i * 4 + 2]);
		mean_luma += 0.2126 * r + 0.7152 * g + 0.0722 * b;
	}
	d["mean_luma"] = mean_luma / static_cast<double>(pixels);
	std::set<uint16_t> rows;
	for (int y = 0; y < h; y++) rows.insert(values[(y * w + w / 2) * 4 + 2]);
	d["distinct_rows"] = static_cast<int>(rows.size());

	// Bucket the frame's SURFACE pixels by view distance, then report the mean luminance of
	// the NEAREST and FARTHEST quartile. The cel rim is driven by grazing angle, and on open
	// ground grazing angle is a proxy for DISTANCE -- which is how a silhouette stylization
	// came to paint a distance ramp across the far field. These two means are what lets a
	// test assert the thing the eye actually complains about: far ground must not out-brighten
	// near ground.
	//
	// Quartiles rather than fixed metre bands because the marched near field only reaches as
	// far as the resident bricks allow, which varies with altitude and with what has streamed
	// in; a fixed band silently empties and takes the assertion with it. The quartiles are
	// always populated when there is geometry at all, and near_dist/far_dist are reported so a
	// test can still pin that the frame spans a real range instead of passing on a flat wall.
	//
	// Sky is excluded by the depth attachment, which is reverse-Z (the passes use
	// COMPARE_OP_GREATER_OR_EQUAL), so 0.0 is the far plane and is what a miss writes.
	const PackedByteArray depth_data = device->texture_get_data(world_->context().render->passes().gbuffer->depth(), 0);
	if (depth_data.size() >= static_cast<int64_t>(pixels) * 4) {
		const float *depths = reinterpret_cast<const float *>(depth_data.ptr());
		std::vector<std::pair<double, double>> surface; // (view distance, luminance)
		surface.reserve(static_cast<size_t>(pixels));
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				const int i = y * w + x;
				if (depths[i] <= 0.0f) continue;
				// The same reconstruction deferred.comp.glsl does, from the same matrix.
				const Vector4 clip(((x + 0.5) / w) * 2.0 - 1.0, ((y + 0.5) / h) * 2.0 - 1.0,
						depths[i], 1.0);
				const Vector4 hw = inv.xform(clip);
				if (std::fabs(hw.w) < 1e-9) continue;
				const Vector3 wp(hw.x / hw.w, hw.y / hw.w, hw.z / hw.w);
				surface.emplace_back(static_cast<double>(wp.distance_to(pos)),
						0.2126 * half_to_float(values[i * 4]) +
								0.7152 * half_to_float(values[i * 4 + 1]) +
								0.0722 * half_to_float(values[i * 4 + 2]));
			}
		}
		d["surface_pixels"] = static_cast<int>(surface.size());
		const size_t quartile = surface.size() / 4;
		if (quartile > 0) {
			std::sort(surface.begin(), surface.end());
			double near_luma = 0.0, far_luma = 0.0, near_dist = 0.0, far_dist = 0.0;
			for (size_t i = 0; i < quartile; i++) {
				near_dist += surface[i].first;
				near_luma += surface[i].second;
				far_dist += surface[surface.size() - 1 - i].first;
				far_luma += surface[surface.size() - 1 - i].second;
			}
			const double n = static_cast<double>(quartile);
			d["near_pixels"] = static_cast<int>(quartile);
			d["far_pixels"] = static_cast<int>(quartile);
			d["near_dist"] = near_dist / n;
			d["far_dist"] = far_dist / n;
			d["near_luma"] = near_luma / n;
			d["far_luma"] = far_luma / n;
		}
	}
	return d;
}

// Marches at a FRACTION of the target size and composites into a full-size G-buffer -- the
// production near-field path, with near_field_scale made explicit. The reported detail is the
// mean absolute albedo step between horizontally adjacent surface pixels: the high-frequency
// energy a magnifying upsample destroys. Sky pixels (composite depth 0) are excluded, as is
// any pair that straddles a silhouette, so the number measures texture, not edges.
Dictionary VoxelDebugHooks::debug_near_field_detail(Vector3 pos, Vector3 fwd, int w, int h,
		float march_scale) {
	Dictionary d;
	d["ran"] = false;
	d["detail"] = 0.0f;
	d["mean_luma"] = 0.0f;
	d["hit_pixels"] = 0;
	d["march_width"] = 0;
	d["march_height"] = 0;
	if (w <= 1 || h <= 1 || !(march_scale > 0.0f) || march_scale > 1.0f) return d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials ||
			!world_->context().render->passes().raymarch || !world_->context().render->passes().composite || !world_->context().render->passes().gbuffer) return d;
	int quiet = 0;
	for (int i = 0; i < 400 && quiet < 6; i++)
		quiet = debug_stream_frame(pos) == 0 ? quiet + 1 : 0;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const float fov_y = 1.0471975512f;
	const ve::ProbeCamera pc = ve::probe_camera(p, f, w, h, fov_y, 0.05f, 4000.0f);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = pc.lod.view_proj[c * 4 + r];
	ve::CameraParams cp{};
	for (int axis = 0; axis < 3; ++axis) {
		cp.cam_pos[axis] = p[axis];
		cp.cam_right[axis] = pc.right[axis];
		cp.cam_up[axis] = pc.up[axis];
		cp.cam_fwd[axis] = pc.fwd[axis];
	}
	cp.params[0] = pc.tan_x;
	cp.params[1] = pc.tan_y;
	cp.params[2] = 200.0f;
	ve::set_near_field_world(&cp, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	ve::set_near_field_flags(&cp, ve::pack_flags(world_->context().render->beauty_settings()));

	const int rw = std::max(1, static_cast<int>(static_cast<float>(w) * march_scale));
	const int rh = std::max(1, static_cast<int>(static_cast<float>(h) * march_scale));
	// The G-buffer and the marcher's targets both change size across calls; the composite's
	// framebuffer references the G-buffer, so drop it before it moves; its uniform set rebuilds
	// on the marcher's new RIDs.
	world_->context().render->passes().composite->release_targets();
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(w, h))) return d;
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cp,
			rw, rh, kNoEdit, world_->context().render->passes().field_context)) return d;
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	world_->context().lod->fade_band(&fade_start, &fade_end);
	world_->context().render->passes().composite->draw(device, *world_->context().render->passes().gbuffer, world_->context().render->passes().raymarch->albedo_texture(),
			world_->context().render->passes().raymarch->surface_texture(), world_->context().render->passes().raymarch->hitpos_texture(),
			view_proj, *world_->context().render->passes().materials, cp, fade_start, fade_end);
	if (!world_->context().render->passes().composite->last_draw_ok()) return d;
	device->submit();
	device->sync();

	const PackedByteArray albedo = device->texture_get_data(world_->context().render->passes().gbuffer->albedo(), 0);
	const PackedByteArray depth = device->texture_get_data(world_->context().render->passes().gbuffer->depth(), 0);
	const int64_t pixels = static_cast<int64_t>(w) * h;
	if (albedo.size() < pixels * 4 || depth.size() < pixels * 4) return d;
	const uint8_t *a = albedo.ptr();
	const float *z = reinterpret_cast<const float *>(depth.ptr());
	// Reverse-Z: a composited surface writes a positive depth, a sky pixel writes exactly 0.
	auto is_surface = [&](int64_t i) { return z[i] > 0.0f; };
	double sum_step = 0.0;
	int64_t steps = 0;
	double mean_luma = 0.0;
	int64_t hits = 0;
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			const int64_t i = static_cast<int64_t>(y) * w + x;
			if (!is_surface(i)) continue;
			hits++;
			mean_luma += (0.2126 * a[i * 4] + 0.7152 * a[i * 4 + 1] + 0.0722 * a[i * 4 + 2]) / 255.0;
			if (x + 1 >= w) continue;
			const int64_t j = i + 1;
			if (!is_surface(j)) continue;
			sum_step += (std::abs(static_cast<int>(a[i * 4]) - static_cast<int>(a[j * 4])) +
					std::abs(static_cast<int>(a[i * 4 + 1]) - static_cast<int>(a[j * 4 + 1])) +
					std::abs(static_cast<int>(a[i * 4 + 2]) - static_cast<int>(a[j * 4 + 2]))) / 255.0;
			steps++;
		}
	}
	// The centre pixel's resolved G-buffer values. The albedo and the gloss are PRODUCED by
	// the composite now, so this is where a test can read the values the deferred pass will
	// light -- the marcher's own targets no longer hold a colour.
	{
		const int64_t c = (static_cast<int64_t>(h / 2)) * w + (w / 2);
		d["center_albedo"] = Color(a[c * 4] / 255.0f, a[c * 4 + 1] / 255.0f,
				a[c * 4 + 2] / 255.0f, 1.0f);
		d["center_sun"] = a[c * 4 + 3] / 255.0f;
		d["center_hit"] = is_surface(c);
		// Reconstructed exactly the way deferred.comp.glsl reconstructs it, from the same
		// depth attachment, so a caller comparing this against a material probe compares the
		// point the deferred pass will actually shade -- not a nearby one.
		const Projection inv_view_proj = view_proj.inverse();
		const Vector2 c_ndc(((w / 2) + 0.5f) / static_cast<float>(w) * 2.0f - 1.0f,
				((h / 2) + 0.5f) / static_cast<float>(h) * 2.0f - 1.0f);
		const Vector4 c_h = inv_view_proj.xform(Vector4(c_ndc.x, c_ndc.y, z[c], 1.0f));
		const float c_w = std::fabs(c_h.w) < 1e-9f ? 1e-9f : c_h.w;
		d["center_position"] = Vector3(c_h.x / c_w, c_h.y / c_w, c_h.z / c_w);
		const PackedByteArray surface = device->texture_get_data(world_->context().render->passes().gbuffer->surface(), 0);
		if (surface.size() >= pixels * 8) {
			const uint16_t *sv = reinterpret_cast<const uint16_t *>(surface.ptr());
			d["center_material"] = static_cast<int>(half_to_float(sv[c * 4 + 2]) + 0.5f);
			d["center_gloss"] = half_to_float(sv[c * 4 + 3]);
			const float e[2] = {half_to_float(sv[c * 4]), half_to_float(sv[c * 4 + 1])};
			float n[3];
			ve::oct_decode(e, n);
			d["center_normal"] = Vector3(n[0], n[1], n[2]);
		}
		// The MARCHER's own normal for the very same pixel, before composite.frag.glsl bent
		// it with the material normal map. Only meaningful when the march ran at full
		// resolution -- below that the marcher's pixel c does not exist.
		if (rw == w && rh == h) {
			const PackedByteArray march =
					device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
			if (march.size() >= pixels * 8) {
				const uint16_t *mv = reinterpret_cast<const uint16_t *>(march.ptr());
				const float e[2] = {half_to_float(mv[c * 4]), half_to_float(mv[c * 4 + 1])};
				float n[3];
				ve::oct_decode(e, n);
				d["center_geometric_normal"] = Vector3(n[0], n[1], n[2]);
			}
			// How much relief the SHIPPED art actually produces, and what it costs the
			// outline pass. `normal_tilt_mean` is the mean 1 - dot(shading, geometric) over
			// every covered pixel: zero means the map changed nothing anywhere, which is the
			// state this whole path existed to leave. `normal_edge_fraction` is the fraction
			// of adjacent covered pairs whose normals differ by more than the outline pass's
			// own threshold -- every one of those is a pixel outline.comp.glsl would darken,
			// so it is the speckle budget for turning the map on.
			const PackedByteArray shaded =
					device->texture_get_data(world_->context().render->passes().gbuffer->surface(), 0);
			if (march.size() >= pixels * 8 && shaded.size() >= pixels * 8) {
				const uint16_t *mv = reinterpret_cast<const uint16_t *>(march.ptr());
				const uint16_t *gv = reinterpret_cast<const uint16_t *>(shaded.ptr());
				auto decode = [](const uint16_t *v, int64_t i, float n[3]) {
					const float e[2] = {half_to_float(v[i * 4]), half_to_float(v[i * 4 + 1])};
					ve::oct_decode(e, n);
				};
				double tilt = 0.0;
				int64_t tilted = 0;
				int64_t pairs = 0, edges = 0;
				for (int y = 0; y < h; y++) {
					for (int x = 0; x < w; x++) {
						const int64_t i = static_cast<int64_t>(y) * w + x;
						if (!is_surface(i)) continue;
						float g[3], m[3];
						decode(gv, i, g);
						decode(mv, i, m);
						tilt += 1.0 - (g[0] * m[0] + g[1] * m[1] + g[2] * m[2]);
						tilted++;
						if (x + 1 >= w || !is_surface(i + 1)) continue;
						float g1[3];
						decode(gv, i + 1, g1);
						pairs++;
						if (1.0f - (g[0] * g1[0] + g[1] * g1[1] + g[2] * g1[2]) >
								world_->context().render->beauty_settings().outline_normal_threshold)
							edges++;
					}
				}
				d["normal_tilt_mean"] = tilted > 0 ? tilt / static_cast<double>(tilted) : 0.0;
				d["normal_edge_fraction"] = pairs > 0
						? static_cast<double>(edges) / static_cast<double>(pairs) : 0.0;
			}
		}
	}
	d["ran"] = true;
	d["march_width"] = rw;
	d["march_height"] = rh;
	d["hit_pixels"] = static_cast<int>(hits);
	d["mean_luma"] = hits > 0 ? mean_luma / static_cast<double>(hits) : 0.0;
	d["detail"] = steps > 0 ? sum_step / static_cast<double>(steps) : 0.0;
	return d;
}

String VoxelDebugHooks::debug_load_shader(const String &res_path) const {
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

Dictionary VoxelDebugHooks::debug_shader_reload_stats() {
	Dictionary d;
	// Task 14: the reload bookkeeping moved into RenderOrchestrator; one-line delegation
	// keeps the single-mutex-hold snapshot shape of the pre-move body.
	int count = 0;
	bool last_ok = true;
	String last_error;
	world_->context().render->reload_snapshot(&count, &last_ok, &last_error);
	d["reloads"] = count;
	d["last_ok"] = last_ok;
	d["last_error"] = last_error;
	return d;
}

void VoxelDebugHooks::debug_set_shader_override(const String &name, const String &source) {
	ve::set_shader_source_override(name.utf8().get_data(), source.utf8().get_data());
}

void VoxelDebugHooks::debug_request_shader_reload() {
	world_->request_shader_reload();
}

void VoxelDebugHooks::debug_clear_shader_source_overrides() {
	ve::clear_shader_source_overrides();
}

Dictionary VoxelDebugHooks::debug_material_atlas_stats() {
	Dictionary d;
	if (!world_->context().render->passes().materials || !world_->context().render->passes().materials->is_valid()) return d;
	d["layers"] = world_->context().render->passes().materials->layer_count();
	d["width"] = kMaterialTextureSize;
	d["height"] = kMaterialTextureSize;
	d["mipmaps"] = kMaterialMipmaps;
	d["albedo_valid"] = world_->context().render->passes().materials->albedo_array().is_valid();
	d["surface_valid"] = world_->context().render->passes().materials->surface_array().is_valid();
	return d;
}

Dictionary VoxelDebugHooks::debug_material_alpha_stats(int layer) {
	Dictionary d;
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().materials ||
			!world_->context().render->passes().materials->is_valid()) return d;
	if (layer < 0 || layer >= world_->context().render->passes().materials->layer_count()) return d;
	const PackedByteArray data =
			device->texture_get_data(world_->context().render->passes().materials->albedo_array(), layer);
	const int64_t top = static_cast<int64_t>(kMaterialTextureSize) * kMaterialTextureSize * 4;
	if (data.size() < top) return d;
	const uint8_t *p = data.ptr();
	uint8_t lo = 255, hi = 0;
	for (int64_t i = 3; i < top; i += 4) { // alpha is every 4th byte
		if (p[i] < lo) lo = p[i];
		if (p[i] > hi) hi = p[i];
	}
	d["min"] = lo / 255.0f;
	d["max"] = hi / 255.0f;
	return d;
}

// Task 7: rewrite the top-mip normal-map texels (surface array RG) of one material layer
// with a hard tilt, so a test can re-render and prove the G-buffer normal never depends
// on the material normal map.
bool VoxelDebugHooks::debug_poke_material_normal(int layer) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().materials) return false;
	if (layer < 0 || layer >= world_->context().render->passes().materials->layer_count()) return false;
	PackedByteArray data = device->texture_get_data(world_->context().render->passes().materials->surface_array(), layer);
	if (data.size() < 4) return false;
	// EVERY texel of every mip in the layer, not just the first one: a probe ray is
	// vanishingly unlikely to land on one poked texel, so a single-texel poke made the
	// invariance assertion pass whether or not the shader sampled these bytes. The surface
	// array is R8G8B8A8 with normal XY in RG (roughness in B, AO in A), so only RG move.
	uint8_t *bytes = data.ptrw();
	for (int64_t i = 0; i + 1 < data.size(); i += 4) {
		bytes[i] = 255; // normal XY = (1, 0): the strongest tilt the format can hold
		bytes[i + 1] = 0;
	}
	device->texture_update(world_->context().render->passes().materials->surface_array(), layer, data);
	return true;
}

// The other half of the poke: a FLAT map (tangent normal +Z) over every texel and mip of one
// layer. A shading normal built by the whiteout blend must come back to the geometric normal
// under it, which is the property that makes applying the map at full strength safe -- an
// unauthored or averaged-away normal may not tilt a surface it says nothing about.
bool VoxelDebugHooks::debug_flatten_material_normal(int layer) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().materials) return false;
	if (layer < 0 || layer >= world_->context().render->passes().materials->layer_count()) return false;
	PackedByteArray data = device->texture_get_data(world_->context().render->passes().materials->surface_array(), layer);
	if (data.size() < 4) return false;
	uint8_t *bytes = data.ptrw();
	for (int64_t i = 0; i + 1 < data.size(); i += 4) {
		bytes[i] = 128; // the closest an 8-bit unorm gets to XY = (0, 0)
		bytes[i + 1] = 128;
	}
	device->texture_update(world_->context().render->passes().materials->surface_array(), layer, data);
	return true;
}

bool VoxelDebugHooks::probe_material(int mat, Vector3 p, Vector3 n, float rgb[3],
		float *roughness, float *ao, float *shading_normal) {
	world_->ensure_initialized();
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().atlas || !world_->context().render->passes().materials || !world_->context().render->passes().raymarch)
		return false;
	const Vector3 forward = n.normalized();
	const float f[3] = {forward.x, forward.y, forward.z};
	float up[3];
	ve::probe_up_hint(f, up);
	ve::CameraParams cam = ve::CameraParams::looking_at(
			p.x, p.y, p.z, f[0], f[1], f[2], up[0], up[1], up[2]);
	// pc.params.w is the debug-probe flag in raymarch.comp.glsl; cam_pos and cam_fwd carry
	// the sample point and normal.
	cam.params[0] = 0.0f;
	cam.params[1] = 0.0f;
	cam.params[2] = 0.0f;
	cam.params[3] = static_cast<float>(mat);
	ve::set_near_field_world(&cam, world_->context().store->region_window(), world_->context().render->island_slot_count(),
			world_->context().store->config().atlas_bricks);
	static const float kNoEdit[6] = {0, 0, 0, 0, 0, 0};
	if (!world_->context().render->passes().raymarch->render(device, *world_->context().render->passes().atlas, world_->context().render->passes().islands, RID(), cam, 1, 1,
			kNoEdit, world_->context().render->passes().field_context))
		return false;
	device->submit();
	device->sync();
	const PackedByteArray data = device->texture_get_data(world_->context().render->passes().raymarch->albedo_texture(), 0);
	const PackedByteArray sf = device->texture_get_data(world_->context().render->passes().raymarch->surface_texture(), 0);
	if (data.size() < 4 || sf.size() < 8) return false;
	const uint8_t *b = data.ptr();
	rgb[0] = b[0] / 255.0f;
	rgb[1] = b[1] / 255.0f;
	rgb[2] = b[2] / 255.0f;
	// The probe path parks material_props() in the two oct slots -- it has no normal to pack
	// there -- so one dispatch answers both halves of the material.
	const uint16_t *s = reinterpret_cast<const uint16_t *>(sf.ptr());
	if (roughness) *roughness = half_to_float(s[0]);
	if (ao) *ao = half_to_float(s[1]);
	if (shading_normal) {
		const PackedByteArray hp =
				device->texture_get_data(world_->context().render->passes().raymarch->hitpos_texture(), 0);
		if (hp.size() < 16) return false;
		const float *h = reinterpret_cast<const float *>(hp.ptr());
		shading_normal[0] = h[0];
		shading_normal[1] = h[1];
		shading_normal[2] = h[2];
	}
	return true;
}

Vector3 VoxelDebugHooks::debug_material_normal_probe(int mat, Vector3 p, Vector3 n) {
	float rgb[3] = {1.0f, 0.0f, 1.0f};
	float sn[3] = {0.0f, 0.0f, 0.0f};
	if (!probe_material(mat, p, n, rgb, nullptr, nullptr, sn)) return Vector3();
	return Vector3(sn[0], sn[1], sn[2]);
}

Color VoxelDebugHooks::resolve_near_field(int mat, Vector3 p, Vector3 n, Color overlay,
		float overlay_weight, float *gloss_out) {
	if (gloss_out) *gloss_out = 0.0f;
	// Material 0 is a miss: the overlay IS the pixel, which is how the sky reaches the screen.
	if (mat <= 0) return Color(overlay.r, overlay.g, overlay.b, 1.0f);
	float rgb[3] = {1.0f, 0.0f, 1.0f};
	float roughness = 1.0f;
	float ao = 1.0f;
	if (!probe_material(mat, p, n, rgb, &roughness, &ao)) return Color(1, 0, 1);
	if (gloss_out) *gloss_out = 1.0f - roughness;
	// The composite's own two lines, mirrored: the AO fold on the material, then the overlay
	// over the result. Keeping the 0.65 in step with composite.frag.glsl is what makes a probe
	// comparable to a pixel.
	const float fold = 1.0f + (ao - 1.0f) * 0.65f;
	const float w = std::min(std::max(overlay_weight, 0.0f), 1.0f);
	return Color(rgb[0] * fold * (1.0f - w) + overlay.r * w,
			rgb[1] * fold * (1.0f - w) + overlay.g * w,
			rgb[2] * fold * (1.0f - w) + overlay.b * w, 1.0f);
}

Color VoxelDebugHooks::debug_material_probe(int mat, Vector3 p, Vector3 n) {
	float rgb[3] = {1.0f, 0.0f, 1.0f};
	if (!probe_material(mat, p, n, rgb, nullptr, nullptr)) return Color(1, 0, 1);
	return Color(rgb[0], rgb[1], rgb[2], 1.0);
}

PackedStringArray VoxelDebugHooks::debug_teardown_trace() {
	PackedStringArray out;
	for (const char *step : world_->context().render->teardown_trace()) out.push_back(step);
	return out;
}

void VoxelDebugHooks::debug_pump_shader_reload() {
	world_->context().render->pump_shader_reload();
}
// --- Task 6 hooks: fixed-capacity stored-normal pool ---
// Debug initializer: shrink the normal-pool budget BEFORE debug_init_atlas(). The
// pool's size is otherwise fixed at exactly 32 MiB and never resizes.
void VoxelDebugHooks::debug_set_normal_pool_budget(int bytes) {
	world_->context().render->set_normal_pool_bytes(bytes > 0 ? static_cast<uint32_t>(bytes) : 0u);
}
RenderingDevice *VoxelDebugHooks::debug_local_rd() const {
	return world_->context().render->local_rd();
}
} // namespace godot
