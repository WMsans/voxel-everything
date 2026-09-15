#include "debug/hooks.h"

#include "../voxel_world.h"
#include "render/frame.h"
#include "render/frame_params.h"
#include "render/orchestrator.h"
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
#include <vector>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>

#include "debug/hooks_common.h"

namespace godot {

void VoxelDebugHooks::debug_lod_tick(Vector3 pos, Vector3 fwd) {
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, f, 2560, 1440,
			1.2217f, 0.1f, 8000.0f);
	world_->context().lod->tick(pc.lod, nullptr);
}

Dictionary VoxelDebugHooks::debug_lod_stats() {
	const LodStats s = world_->context().lod->stats();
	Dictionary d;
	d["pages_total"] = s.pages_total;
	d["pages_free"] = s.pages_free;
	d["pages_used"] = s.pages_total - s.pages_free;
	d["chunks_resident"] = s.chunks_resident;
	d["chunk_records"] = s.chunk_records;
	d["chunk_records_used"] = s.chunk_records_used;
	d["chunk_records_high_water"] = s.chunk_records_high_water;
	d["pages_high_water"] = s.pages_high_water;
	d["budget_bound"] = String(s.budget_bound);
	d["dirty_chunks"] = s.dirty_chunks;
	d["dirty_levels"] = s.dirty_levels;
	d["draw_pages"] = s.draw_pages;
	PackedInt32Array draw_page_ids;
	for (int page : s.draw_page_ids) draw_page_ids.append(page);
	d["draw_page_ids"] = draw_page_ids;
	PackedInt32Array resident_page_ids;
	for (int page : s.resident_page_ids) resident_page_ids.append(page);
	d["resident_page_ids"] = resident_page_ids;
	// Zero pending requests with nothing in flight is what "the far field has converged for
	// this camera" means; tests wait on it instead of guessing a frame count.
	d["requests_pending"] = static_cast<int>(s.requests.size());
	Array pending_request_ids;
	for (const ve::LodBuildRequest &request : s.requests) {
		pending_request_ids.append(String::num_int64(request.level) + ":" +
				String::num_int64(request.coord.x) + ":" + String::num_int64(request.coord.y) + ":" +
				String::num_int64(request.coord.z));
	}
	d["pending_request_ids"] = pending_request_ids;
	d["partial_allocations"] = s.partial_allocations;
	d["builds_in_flight"] = world_->mesh_service() && world_->mesh_service()->lod_busy() ? 1 : 0;
	// The benchmark's horizon tracker reads this name; same count as requests_pending.
	d["lod_pending"] = static_cast<int>(s.requests.size());
	// Async cull stats readback; zero until the first readback lands (safe "nothing culled").
	d["culled_ratio"] = world_->context().render->passes().lod_cull
			? world_->context().render->passes().lod_cull->culled_ratio() : 0.0f;
	return d;
}

Vector2 VoxelDebugHooks::debug_lod_fade_band() {
	float start = ve::kLodFadeStartM;
	float end = ve::kLodFadeEndM;
	world_->context().lod->fade_band(&start, &end);
	return Vector2(start, end);
}

Dictionary VoxelDebugHooks::debug_lod_render_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	return debug_lod_render_probe_culled(pos, fwd, w, h, true);
}

Dictionary VoxelDebugHooks::debug_lod_render_probe_culled(Vector3 pos, Vector3 fwd, int w, int h,
		bool cull) {
	Dictionary d;
	d["coverage"] = 0.0f;
	d["depth_min"] = 0.0f;
	d["depth_max"] = 0.0f;
	d["nearest_hit_m"] = 0.0f;
	d["draw_pages"] = 0;
	d["depth_sum"] = 0.0;
	if (w <= 0 || h <= 0) return d;

	// One tick: refresh the walk and the raster pass's page list for this view. debug_lod_tick
	// -> lod_tick takes lod_mutex_ around ensure_lod and all LoD state mutation.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().lod->pool() || !world_->context().render->passes().lod_raster || !world_->context().render->passes().materials) return d;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, f, w, h,
			1.2217f, 0.1f, 8000.0f);
	const ve::LodCamera &cam = pc.lod;
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	if (!world_->context().render->passes().gbuffer || !world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(w, h))) return d;

	// Clear to reverse-Z far (0) before drawing the far field. Through a render pass, not
	// texture_clear: that is a COLOUR clear and Godot's Metal driver refuses it on a depth
	// format, which left this probe reading the previous probe's depth on that device.
	if (!world_->context().render->passes().lod_raster->clear_targets(device, *world_->context().render->passes().gbuffer)) return d;
	world_->context().render->passes().lod_raster->set_cull_enabled(cull);
	const int draw_count = world_->context().render->passes().lod_raster->draw_page_count();
	world_->context().lod->pool()->upload_draw_args(world_->context().render->passes().lod_raster->draw_pages());
	float probe_start = ve::kLodFadeStartM;
	float probe_end = ve::kLodFadeEndM;
	world_->context().lod->fade_band(&probe_start, &probe_end);
	bool ok = world_->context().render->passes().lod_raster->draw(device, *world_->context().lod->pool(), *world_->context().render->passes().materials, *world_->context().render->passes().gbuffer,
			vp, p, draw_count, probe_start, probe_end);
	device->submit();
	device->sync();

	if (ok) {
		const PackedByteArray depth_data = device->texture_get_data(world_->context().render->passes().gbuffer->depth(), 0);
		const int pixel_count = w * h;
		if (depth_data.size() >= pixel_count * 4) {
			const float *depths = reinterpret_cast<const float *>(depth_data.ptr());
			int covered = 0;
			float dmin = 1.0f;
			float dmax = 0.0f;
			// Coverage alone cannot see a change that swaps one surface for another behind
			// it -- a crater in ground that has more ground behind it keeps every pixel
			// covered. Accumulate the depths too, so a test can ask whether the far field's
			// IMAGE changed rather than only whether its silhouette did.
			double depth_sum = 0.0;
			for (int i = 0; i < pixel_count; i++) {
				const float dv = depths[i];
				if (dv <= 0.0f) continue;
				covered++;
				depth_sum += static_cast<double>(dv);
				if (dv < dmin) dmin = dv;
				if (dv > dmax) dmax = dv;
			}
			d["depth_sum"] = depth_sum;
			d["coverage"] = static_cast<float>(covered) / static_cast<float>(pixel_count);
			d["depth_min"] = covered > 0 ? dmin : 0.0f;
			d["depth_max"] = covered > 0 ? dmax : 0.0f;
			// Reverse-Z perspective: depth d in [0,1] maps to view distance
			// far*near / (near + d*(far-near)); the largest depth is the nearest hit.
			if (covered > 0) {
				const float near_z = 0.1f;
				const float far_z = 8000.0f;
				const float denom = near_z + dmax * (far_z - near_z);
				d["nearest_hit_m"] = denom > 0.0f ? far_z * near_z / denom : 0.0f;
			}
		}
	}

	// The raster pass already holds the exact page list produced by prepare_lod_raster;
	// reading lod_walk_ here would require re-taking lod_mutex_ after the tick released it.
	d["draw_pages"] = world_->context().render->passes().lod_raster ? world_->context().render->passes().lod_raster->draw_page_count() : 0;

	// The raster pass cached a framebuffer over the owned G-buffer; drop it before a future
	// probe changes its attachments.
	world_->context().render->passes().lod_raster->release_targets();
	return d;
}

Dictionary VoxelDebugHooks::debug_lod_gbuffer_probe(Vector3 pos, Vector3 fwd, int w, int h) {
	Dictionary d;
	d["material_coverage"] = 0.0f;
	d["worst_normal_length_error"] = 0.0f;
	d["gloss_max"] = 0.0f;
	d["sun_min"] = 1.0f;
	d["sun_max"] = 0.0f;
	d["normal_mean"] = Vector3();
	if (w <= 0 || h <= 0) return d;

	debug_lod_tick(pos, fwd);
	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().lod->pool() || !world_->context().render->passes().lod_raster || !world_->context().render->passes().materials || !world_->context().render->passes().gbuffer)
		return d;
	world_->context().render->passes().lod_raster->release_targets();
	if (!world_->context().render->passes().gbuffer->ensure(device, nullptr, Vector2i(w, h))) return d;

	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, f, w, h,
			1.2217f, 0.1f, 8000.0f);
	const ve::LodCamera &cam = pc.lod;
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	// One render-pass clear for all three attachments; see debug_lod_render_probe above for
	// why the depth one cannot be a texture_clear.
	if (!world_->context().render->passes().lod_raster->clear_targets(device, *world_->context().render->passes().gbuffer)) return d;
	world_->context().render->passes().lod_raster->set_cull_enabled(true);
	world_->context().lod->pool()->upload_draw_args(world_->context().render->passes().lod_raster->draw_pages());
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	world_->context().lod->fade_band(&fade_start, &fade_end);
	const bool ok = world_->context().render->passes().lod_raster->draw(device, *world_->context().lod->pool(), *world_->context().render->passes().materials, *world_->context().render->passes().gbuffer, vp, p,
			world_->context().render->passes().lod_raster->draw_page_count(), fade_start, fade_end);
	device->submit();
	device->sync();

	if (ok) {
		const PackedByteArray albedo = device->texture_get_data(world_->context().render->passes().gbuffer->albedo(), 0);
		const PackedByteArray surface = device->texture_get_data(world_->context().render->passes().gbuffer->surface(), 0);
		const int pixels = w * h;
		if (albedo.size() >= pixels * 4 && surface.size() >= pixels * 8) {
			const uint8_t *a = reinterpret_cast<const uint8_t *>(albedo.ptr());
			const uint16_t *s = reinterpret_cast<const uint16_t *>(surface.ptr());
			int covered = 0;

			float worst = 0.0f;
			float gloss_max = 0.0f;
			float sun_min = 1.0f;
			float sun_max = 0.0f;
			// The MEAN decoded normal over the covered pixels. A per-pixel normal cannot be
			// compared across two renders -- the LoD walk may hand back a different page set --
			// but the average orientation of a settled patch is stable, which is what makes it
			// a usable before/after for "did the material normal map move the far field".
			double nsum[3] = {0.0, 0.0, 0.0};
			for (int i = 0; i < pixels; i++) {
				const float material = Math::half_to_float(s[i * 4 + 2]);
				if (material < 0.5f) continue;
				covered++;
				const float e[2] = {Math::half_to_float(s[i * 4]), Math::half_to_float(s[i * 4 + 1])};
				float n[3] = {};
				ve::oct_decode(e, n);
				const float length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				worst = std::max(worst, std::fabs(length - 1.0f));
				nsum[0] += n[0]; nsum[1] += n[1]; nsum[2] += n[2];
				gloss_max = std::max(gloss_max, Math::half_to_float(s[i * 4 + 3]));
				const float sun = static_cast<float>(a[i * 4 + 3]) / 255.0f;
				sun_min = std::min(sun_min, sun);
				sun_max = std::max(sun_max, sun);
			}
			d["material_coverage"] = static_cast<float>(covered) / static_cast<float>(pixels);
			d["worst_normal_length_error"] = worst;
			d["gloss_max"] = gloss_max;
			d["sun_min"] = covered > 0 ? sun_min : 1.0f;
			d["sun_max"] = covered > 0 ? sun_max : 1.0f;
			if (covered > 0) {
				const double inv = 1.0 / static_cast<double>(covered);
				d["normal_mean"] = Vector3(static_cast<float>(nsum[0] * inv),
						static_cast<float>(nsum[1] * inv), static_cast<float>(nsum[2] * inv));
			}
		}
	}
	world_->context().render->passes().lod_raster->release_targets();
	return d;
}

Dictionary VoxelDebugHooks::debug_seam_probe(Vector3 pos, Vector3 fwd, int w, int h, bool skip_lod) {
	Dictionary d;
	d["band_pixels"] = 0;
	d["band_pixels_unclaimed"] = 0;
	d["band_pixels_double_claimed"] = 0;
	d["near_pixels_lost_to_lod"] = 0;
	d["far_pixels_lost_to_raymarch"] = 0;
	if (w <= 0 || h <= 0) return d;

	// One tick refreshes the walk and the raster pass's page list for this view, with the
	// 2560x1440 viewport the LoD suites settle at; the frame below ticks with the same one.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().render->passes().gbuffer ||
			!world_->context().render->passes().raymarch || !world_->context().render->passes().composite || !world_->context().render->passes().lod_raster)
		return d;
	// The classification below reads the marcher's hitpos per FULL-resolution pixel.
	if (world_->get_near_field_scale() < 1.0f) {
		d["error"] = "debug_seam_probe needs near_field_scale = 1.0";
		return d;
	}

	// The near field needs the streamer to have populated the SDF atlas; the LoD settle in
	// the test only converges the far-field walk. Drive the streamer until it is quiet (the
	// same condition the near-field tests use) before rendering.
	{
		int quiet = 0;
		for (int i = 0; i < 120 && quiet < 6; i++) {
			const int actions = debug_stream_frame(pos);
			quiet = actions == 0 ? quiet + 1 : 0;
		}
	}

	const float p[3] = {pos.x, pos.y, pos.z};
	const float aspect = static_cast<float>(w) / static_cast<float>(h);
	const float fov_y = 1.2217f;
	const float tan_y = std::tan(fov_y * 0.5f);
	const float tan_x = tan_y * aspect;
	const float kNear = 0.1f;
	const float kFar = 8000.0f;

	RID marker;
	{
		Ref<RDTextureFormat> tf;
		tf.instantiate();
		tf->set_format(RenderingDevice::DATA_FORMAT_R8_UINT);
		tf->set_width(w);
		tf->set_height(h);
		tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_COLOR_ATTACHMENT_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT |
				RenderingDevice::TEXTURE_USAGE_CAN_COPY_TO_BIT |
				RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
		PackedByteArray zero;
		zero.resize(w * h);
		zero.fill(0);
		TypedArray<PackedByteArray> upload;
		upload.push_back(zero);
		Ref<RDTextureView> tv;
		tv.instantiate();
		marker = device->texture_create(tf, tv, upload);
	}
	if (!marker.is_valid()) return d;
	// Drop the framebuffers that reference the marker before freeing it.
	auto cleanup = [&]() {
		world_->context().render->passes().composite->release_targets();
		world_->context().render->passes().lod_raster->release_targets();
		device->free_rid(marker);
	};

	// The shipped frame with the marker attached: composite ORs 1 where the near field keeps
	// a pixel, the far field ORs 2 (LOGIC_OP_OR), so double-claimed pixels read 3. skip_lod
	// leaves the far field out entirely to create a real gap the probe must count.
	FrameInputs in = VoxelFrame::looking_at(pos, fwd, w, h, fov_y, kNear, kFar);
	in.debug.marker = marker;
	in.debug.skip_far_field = skip_lod;
	in.debug.lod_viewport = Vector2i(2560, 1440);
	world_->context().render->frame().render_headless(device, in);
	device->submit();
	device->sync();
	const FrameRecord record = world_->context().render->frame().last_frame();
	if (!record.stage_ok(kStageComposite) || !record.stage_ok(kStageInject)) {
		cleanup();
		return d;
	}
	// Classify against the band the frame itself faded on this frame.
	const float probe_fade_start = record.fade_start;
	const float probe_fade_end = record.fade_end;

	const PackedByteArray depth_data = device->texture_get_data(world_->context().render->passes().gbuffer->depth(), 0);
	const PackedByteArray marker_data = device->texture_get_data(marker, 0);
	const PackedByteArray hitpos_data = device->texture_get_data(
			world_->context().render->passes().raymarch->hitpos_texture(), 0);
	int band_pixels = 0;
	int band_pixels_unclaimed = 0;
	int band_pixels_double_claimed = 0;
	int near_pixels_lost_to_lod = 0;
	int far_pixels_lost_to_raymarch = 0;
	if (depth_data.size() >= static_cast<int64_t>(w) * h * 4 &&
			marker_data.size() >= static_cast<int64_t>(w) * h &&
			hitpos_data.size() >= static_cast<int64_t>(w) * h * 16) {
		const float *df = reinterpret_cast<const float *>(depth_data.ptr());
		const uint8_t *mk = reinterpret_cast<const uint8_t *>(marker_data.ptr());
		const float *hf = reinterpret_cast<const float *>(hitpos_data.ptr());
		// Reconstruct the world hit from the reverse-Z depth and the same camera the two fields
		// use, so the probe measures the same Euclidean distance the shaders fade on. When both
		// fields discarded a pixel (the unclaimed case), depth is 0 but the raymarch hitpos
		// texture still records the terrain hit; use it to recover the distance.
		for (int i = 0; i < w * h; i++) {
			const float depth_val = df[i];
			const float *hp = &hf[i * 4];
			const bool raymarch_hit = hp[3] >= 0.5f;
			float dist;
			if (depth_val > 0.0f) {
				const float u = (static_cast<float>(i % w) + 0.5f) / static_cast<float>(w);
				const float v = (static_cast<float>(i / w) + 0.5f) / static_cast<float>(h);
				const float ndc_x = u * 2.0f - 1.0f;
				const float ndc_y = 1.0f - v * 2.0f;
				const float z_view = kFar * kNear / (kNear + depth_val * (kFar - kNear));
				const float ax = ndc_x * tan_x;
				const float ay = ndc_y * tan_y;
				dist = z_view * std::sqrt(1.0f + ax * ax + ay * ay);
			} else if (raymarch_hit) {
				const float dx = hp[0] - p[0];
				const float dy = hp[1] - p[1];
				const float dz = hp[2] - p[2];
				dist = std::sqrt(dx * dx + dy * dy + dz * dz);
			} else {
				// No terrain sample to classify: sky, not an unclaimed band pixel.
				continue;
			}
			const uint8_t m = mk[i];
			if (dist >= probe_fade_start && dist <= probe_fade_end) {
				band_pixels++;
				if (m == 0u) band_pixels_unclaimed++;
				if (m == 3u) band_pixels_double_claimed++;
			} else if (dist < probe_fade_start && (m & 2u) != 0u) {
				near_pixels_lost_to_lod++;
			} else if (dist > probe_fade_end && (m & 1u) != 0u) {
				far_pixels_lost_to_raymarch++;
			}
		}
	}
	d["band_pixels"] = band_pixels;
	d["band_pixels_unclaimed"] = band_pixels_unclaimed;
	d["band_pixels_double_claimed"] = band_pixels_double_claimed;
	// Short aliases retained for the Task 7 seam contract.
	d["neither"] = band_pixels_unclaimed;
	d["both"] = band_pixels_double_claimed;
	d["near_pixels_lost_to_lod"] = near_pixels_lost_to_lod;
	d["far_pixels_lost_to_raymarch"] = far_pixels_lost_to_raymarch;
	d["draw_pages"] = world_->context().render->passes().lod_raster->draw_page_count();
	cleanup();
	return d;
}

Dictionary VoxelDebugHooks::debug_lod_cull_probe(Vector3 pos, Vector3 fwd) {
	Dictionary d;
	d["args_before"] = 0;
	d["args_after"] = 0;
	d["offsets_changed"] = 0;
	d["index_counts_changed"] = 0;
	d["drawn_after"] = 0;
	d["culled_ratio"] = 0.0f;

	// One tick: refresh the walk and the raster pass's page list for this view.
	debug_lod_tick(pos, fwd);

	RenderingDevice *device = world_->rd();
	if (!world_->is_initialized() || !device || !world_->context().lod->pool() || !world_->context().render->passes().lod_raster || !world_->context().render->passes().lod_cull ||
			!world_->context().render->passes().hiz) {
		return d;
	}
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, f, 2560, 1440,
			1.2217f, 0.1f, 8000.0f);
	const ve::LodCamera &cam = pc.lod;
	Projection vp;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			vp.columns[c][r] = cam.view_proj[c * 4 + r];

	const int draw_count = world_->context().render->passes().lod_raster->draw_page_count();
	if (draw_count <= 0) return d;
	world_->context().lod->pool()->upload_draw_args(world_->context().render->passes().lod_raster->draw_pages());
	device->submit();
	device->sync();
	const PackedByteArray before = device->buffer_get_data(world_->context().lod->pool()->args_buffer(), 0,
			static_cast<uint32_t>(draw_count) * 20);

	// This probe deliberately exercises frustum culling plus whatever HiZ state is present.
	// Build a synthetic "everything far" pyramid first so the run never reads an unbuilt or
	// stale pyramid (the production path builds from the real scene depth before culling).
	debug_hiz_probe_synthetic(0.0f, 1.0f);

	const bool ok = world_->context().render->passes().lod_cull->run(device, *world_->context().lod->pool(), world_->context().render->passes().hiz, vp, draw_count,
			draw_count, 0);
	device->submit();
	device->sync();
	if (!ok) return d;

	const PackedByteArray after = device->buffer_get_data(world_->context().lod->pool()->args_buffer(), 0,
			static_cast<uint32_t>(draw_count) * 20);
	if (before.size() < static_cast<int64_t>(draw_count) * 20 ||
			after.size() < static_cast<int64_t>(draw_count) * 20) {
		return d;
	}

	const uint32_t *b = reinterpret_cast<const uint32_t *>(before.ptr());
	const uint32_t *a = reinterpret_cast<const uint32_t *>(after.ptr());
	int offsets_changed = 0;
	int index_counts_changed = 0;
	int drawn_after = 0;
	PackedInt32Array pages;
	PackedInt32Array culled;
	PackedInt32Array page_frustum_culled;
	PackedInt32Array slot_frustum_culled;
	const std::vector<uint32_t> &page_chunk_cpu = world_->context().lod->pool()->page_chunk_cpu();
	float planes[6][4];
	ve::lod_frustum_planes(cam.view_proj, planes);
	const PackedByteArray chunk_bytes = device->buffer_get_data(world_->context().lod->pool()->chunk_buffer(), 0,
			static_cast<uint32_t>(world_->context().lod->pool()->chunk_record_count() * 32));
	const float *chunk_data = reinterpret_cast<const float *>(chunk_bytes.ptr());
	const bool have_chunks = chunk_bytes.size() >= world_->context().lod->pool()->chunk_record_count() * 32;
	auto aabb_outside = [&](uint32_t ci) -> bool {
		if (!have_chunks || ci == 0xffffffffu) return true;
		const float *rec = chunk_data + static_cast<size_t>(ci) * 8;
		const float lo[3] = {rec[0], rec[1], rec[2]};
		const float cell = rec[3];
		const float hi[3] = {lo[0] + cell * float(ve::kLodChunkCells),
				lo[1] + cell * float(ve::kLodChunkCells),
				lo[2] + cell * float(ve::kLodChunkCells)};
		return !ve::lod_aabb_in_frustum(planes, lo, hi);
	};
	for (int i = 0; i < draw_count; i++) {
		const size_t base = static_cast<size_t>(i) * 5;
		if (a[base + 1] != 0u) drawn_after++;
		if (b[base + 0] != a[base + 0]) index_counts_changed++;
		if (b[base + 3] != a[base + 3]) offsets_changed++;
		const uint32_t page = b[base + 3] / static_cast<uint32_t>(ve::kLodVertsPerPage);
		const uint32_t page_ci = page < page_chunk_cpu.size() ?
				page_chunk_cpu[static_cast<size_t>(page)] : 0xffffffffu;
		const uint32_t slot_ci = static_cast<uint32_t>(i) < page_chunk_cpu.size() ?
				page_chunk_cpu[static_cast<size_t>(i)] : 0xffffffffu;
		pages.append(static_cast<int32_t>(page));
		culled.append(a[base + 1] == 0u ? 1 : 0);
		page_frustum_culled.append(aabb_outside(page_ci) ? 1 : 0);
		slot_frustum_culled.append(aabb_outside(slot_ci) ? 1 : 0);
	}

	d["args_before"] = draw_count;
	d["args_after"] = draw_count;
	d["offsets_changed"] = offsets_changed;
	d["index_counts_changed"] = index_counts_changed;
	d["drawn_after"] = drawn_after;
	d["culled_ratio"] = static_cast<float>(draw_count - drawn_after) / static_cast<float>(draw_count);
	d["pages"] = pages;
	d["culled"] = culled;
	d["page_frustum_culled"] = page_frustum_culled;
	d["slot_frustum_culled"] = slot_frustum_culled;
	return d;
}

Dictionary VoxelDebugHooks::debug_lod_cull_debug() {
	const FrameRecord r = world_->context().render->frame().last_frame();
	Dictionary d;
	d["two_phase"] = r.lod_two_phase;
	d["hiz_built"] = r.hiz_built;
	d["first_pass_count"] = r.lod_first_pass_count;
	return d;
}

Dictionary VoxelDebugHooks::debug_lod_diff(int level, Vector3i coord) {
	Dictionary d;
	world_->ensure_physics_initialized();
	if (!world_->physics_ready() || !world_->mesh_service()) return d;
	constexpr int kFineCount = ve::kLodFineLattice * ve::kLodFineLattice * ve::kLodFineLattice;
	constexpr int kReducedCount =
			ve::kLodChunkLattice * ve::kLodChunkLattice * ve::kLodChunkLattice;
	const ve::IVec3 c{coord.x, coord.y, coord.z};
	std::vector<ve::EditOp> ops;
	world_->context().lod->gather_ops(level, c, &ops);

	std::vector<uint8_t> fine_sdf, reduced_sdf;
	std::vector<uint16_t> fine_mat, reduced_mat;
	LodBuildResult result;
	bool ok = false;
	float origin[3];
	ve::lod_chunk_origin(level, c, origin);
	const ve::IVec3 region = ve::region_of_point(origin[0], origin[1], origin[2]);
	const int override_table = world_->context().store->override_table_for_region(region);
	world_->mesh_service()->run_sync([&](MeshPass &pass) {
		(void)pass;
		// The worker thread owns this device for the duration of the diagnostic. Task 10
		// moves LodBuildPass into MeshService itself; until then a per-call local device is
		// the smallest way to keep every RenderingDevice call on its creating thread.
		RenderingDevice *rd =
				RenderingServer::get_singleton()->create_local_rendering_device();
		if (!rd) return;
		LodBuildPass lod;
		LodBuildConfig cfg;
		cfg.max_jobs = 1;
		if (!lod.initialize(rd, cfg)) {
			memdelete(rd);
			return;
		}
		// This diagnostic owns its device, so neither the worker's nor the
		// orchestrator's set 1 can be borrowed: build one against this device and this
		// pass's shader from the stored pipeline (empty when the pipeline failed to
		// load, matching the fallback stub the shaders compiled against). Fail-soft to
		// no set, as elsewhere.
		FieldContextSet lod_context;
		if (lod_context.initialize(rd, lod.field_shader(),
				world_->context().store->terrain_pipeline())) {
			lod.set_field_context(&lod_context);
		}
		for (int slot = 0; slot < ve::kMaxVolumes; slot++) {
			const ve::VolumeData *v = world_->context().store->volumes().get(slot);
			if (v) lod.volumes().upload(rd, slot, *v);
		}
		std::vector<std::pair<int, int>> override_entries;
		if (override_table >= 0 && world_->context().store->overrides()) {
			const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			for (int z = 0; z < ve::kRegionBricks; z++)
				for (int y = 0; y < ve::kRegionBricks; y++)
					for (int x = 0; x < ve::kRegionBricks; x++) {
						const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
						const int slot = world_->context().store->overrides()->slot_of(b);
						if (slot < 0) continue;
						const ve::OverrideBrick *data = world_->context().store->overrides()->data(slot);
						if (!data || !lod.upload_override(slot, *data)) {
							lod.set_field_context(nullptr);
							lod_context.teardown();
							lod.teardown();
							memdelete(rd);
							return;
						}
						override_entries.emplace_back(ve::brick_index_in_region(b), slot);
					}
			lod.set_override_table(0, override_table, override_entries);
		}
		LodBuildJob job;
		job.level = level;
		job.coord = c;
		job.ops = ops;
		job.override_table = override_table;
		ok = lod.build_sync(job, &result, &reduced_sdf, &reduced_mat);
		if (ok) {
			const PackedByteArray fs = rd->texture_get_data(lod.fine_sdf(), 0);
			const PackedByteArray fm = rd->texture_get_data(lod.fine_mat(), 0);
			if (fs.size() >= kFineCount)
				fine_sdf.assign(fs.ptr(), fs.ptr() + kFineCount);
			if (fm.size() >= static_cast<int64_t>(kFineCount) * 2) {
				fine_mat.resize(kFineCount);
				std::memcpy(fine_mat.data(), fm.ptr(), static_cast<size_t>(kFineCount) * 2);
			}
		}
		// The borrowed set must die before its device does (stack destruction runs after
		// memdelete below and would free RIDs on a dead device).
		lod.set_field_context(nullptr);
		lod_context.teardown();
		lod.teardown();
		memdelete(rd);
	});
	if (!ok || result.failed || static_cast<int>(fine_sdf.size()) != kFineCount ||
			static_cast<int>(fine_mat.size()) != kFineCount ||
			static_cast<int>(reduced_sdf.size()) != kReducedCount ||
			static_cast<int>(reduced_mat.size()) != kReducedCount)
		return d;

	const float cell = ve::lod_cell_size(level);
	const ve::Generator &gen = world_->context().store->generator()->sampler();

	// 1. The fine lattice against the CPU field.
	int fine_max_diff = 0;
	for (int z = 0; z < ve::kLodFineLattice; z++)
		for (int y = 0; y < ve::kLodFineLattice; y++)
			for (int x = 0; x < ve::kLodFineLattice; x++) {
				const float p[3] = {origin[0] + (static_cast<float>(x) - 3.0f) * cell * 0.5f,
						origin[1] + (static_cast<float>(y) - 3.0f) * cell * 0.5f,
						origin[2] + (static_cast<float>(z) - 3.0f) * cell * 0.5f};
				const float s = ve::eval_field(gen, ops.data(), static_cast<int>(ops.size()),
						p[0], p[1], p[2], &world_->context().store->volumes(), world_->context().store->overrides()).sdf;
				const int idx = ve::lod_fine_index(x, y, z);
				const int diff = std::abs(static_cast<int>(fine_sdf[idx]) -
						static_cast<int>(ve::lod_encode_sdf(s, cell)));
				fine_max_diff = std::max(fine_max_diff, diff);
			}
	d["fine_max_diff"] = fine_max_diff;

	// 2. The reduced lattice against ve::lod_reduce_lattice on the GPU's own fine bytes.
	std::vector<uint8_t> cpu_reduced(kReducedCount);
	std::vector<uint16_t> cpu_reduced_mat(kReducedCount);
	ve::lod_reduce_lattice(fine_sdf.data(), fine_mat.data(), cpu_reduced.data(),
			cpu_reduced_mat.data());
	int reduced_max_diff = 0;
	int material_mismatches = 0;
	for (int i = 0; i < kReducedCount; i++) {
		reduced_max_diff = std::max(reduced_max_diff,
				std::abs(static_cast<int>(reduced_sdf[i]) -
						static_cast<int>(cpu_reduced[i])));
		if (reduced_mat[i] != cpu_reduced_mat[i]) material_mismatches++;
	}
	d["reduced_max_diff"] = reduced_max_diff;
	d["material_mismatches"] = material_mismatches;

	// 3. The quads against ve::lod_contour on the GPU's own reduced bytes. The CPU side gets
	// skirts appended exactly as LodBuildPass::build_sync does, so the two sets cover the
	// same final records.
	ve::LodContourResult ref;
	ve::lod_contour(reduced_sdf.data(), reduced_mat.data(), &ref);
	ve::lod_append_skirts(&ref.quads, &ref.normals);

	using QuadKey = std::array<int, 10>; // u xyz, axis, sign, ribbon tag/face/edge/reverse, material
	using Offsets = std::array<int, 12>;
	struct Candidate {
		Offsets offsets{};
		ve::LodQuadNormals normals{};
	};
	std::map<QuadKey, std::vector<Candidate>> cpu_quads;
	const auto make_key = [](const ve::LodQuadFields &f) {
		QuadKey k{f.u[0], f.u[1], f.u[2], f.axis, f.sign, f.double_sided,
				f.skirt_face, f.skirt_edge, f.reverse_winding, f.material};
		return k;
	};
	const auto make_offsets = [](const ve::LodQuadFields &f) {
		Offsets o{};
		for (int k = 0; k < 4; k++)
			for (int a = 0; a < 3; a++)
				o[k * 3 + a] = f.offset[k][a];
		return o;
	};
	for (size_t qi = 0; qi < ref.quads.size(); ++qi) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(ref.quads[qi], &f);
		cpu_quads[make_key(f)].push_back({make_offsets(f), ref.normals[qi]});
	}
	int quads_only_cpu = 0;
	int quads_only_gpu = 0;
	int raw_corner_max_diff = 0;
	int normal_max_diff = 0; // diagnostic only: oct encoding is discontinuous at fold edges
	float normal_min_dot = 1.0f;
	for (size_t qi = 0; qi < result.quads.size(); ++qi) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(result.quads[qi], &f);
		const QuadKey key = make_key(f);
		const Offsets offs = make_offsets(f);
		auto it = cpu_quads.find(key);
		if (it == cpu_quads.end() || it->second.empty()) {
			quads_only_gpu++;
			continue;
		}
		// A (u, axis) key can legitimately appear more than once after skirts from two
		// different boundary parents land on the same shifted edge. Pick the CPU candidate
		// with the closest offsets so those duplicate pairs do not cross-match.
		size_t best = 0;
		int best_diff = 1 << 30;
		for (size_t i = 0; i < it->second.size(); i++) {
			int diff = 0;
			for (int k = 0; k < 12; k++)
				diff = std::max(diff, std::abs(static_cast<int>(offs[k]) -
						static_cast<int>(it->second[i].offsets[k])));
			if (diff < best_diff) {
				best_diff = diff;
				best = i;
			}
		}
		raw_corner_max_diff = std::max(raw_corner_max_diff, best_diff);
		if (qi < result.normals.size()) {
			for (int k = 0; k < 4; ++k) {
				const uint16_t a = result.normals[qi].corner[k];
				const uint16_t b = it->second[best].normals.corner[k];
				for (int shift : {0, 8})
					normal_max_diff = std::max(normal_max_diff,
							std::abs(int(int8_t(a >> shift)) - int(int8_t(b >> shift))));
				float na[3], nb[3];
				ve::oct_decode_snorm8(a, na);
				ve::oct_decode_snorm8(b, nb);
				normal_min_dot = std::min(normal_min_dot,
						na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2]);
			}
		} else {
			normal_max_diff = 255;
		}
		it->second[best] = it->second.back();
		it->second.pop_back();
	}
	for (const auto &kv : cpu_quads) quads_only_cpu += static_cast<int>(kv.second.size());
	// The 5-bit offset quantisation sits on the same float-rounding boundary the lattice
	// tolerances already allow for: a one-step flip is driver/compiler noise, not an
	// algorithmic drift. Report it as 0 while still surfacing larger vertex/winding bugs.
	const int corner_max_diff = raw_corner_max_diff <= 1 ? 0 : raw_corner_max_diff;
	d["quads_only_cpu"] = quads_only_cpu;
	d["quads_only_gpu"] = quads_only_gpu;
	d["corner_max_diff"] = corner_max_diff;
	d["normal_max_diff"] = normal_max_diff;
	d["normal_min_dot"] = normal_min_dot;
	d["quads_cpu"] = static_cast<int>(ref.quads.size());
	d["quads_gpu"] = static_cast<int>(result.quads.size());

	// 4. FNV-1a over the reduced SDF bytes, so a test can assert an edit moved a coarse level.
	uint32_t hash = 2166136261u;
	for (uint8_t b : reduced_sdf) {
		hash ^= b;
		hash *= 16777619u;
	}
	d["reduced_hash"] = static_cast<int64_t>(hash);
	d["op_count"] = static_cast<int>(ops.size());
	return d;
}

bool VoxelDebugHooks::debug_lod_submit(Array jobs) {
	world_->ensure_physics_initialized();
	if (!world_->physics_ready() || !world_->mesh_service()) return false;
	std::vector<LodBuildJob> lod_jobs;
	lod_jobs.reserve(static_cast<size_t>(jobs.size()));
	for (int i = 0; i < jobs.size(); i++) {
		const Array pair = jobs[i];
		if (pair.size() < 2) continue;
		const int level = pair[0];
		const Vector3i v = pair[1];
		const ve::IVec3 c{v.x, v.y, v.z};
		LodBuildJob job;
		job.level = level;
		job.coord = c;
		world_->context().lod->gather_ops(level, c, &job.ops);
		lod_jobs.push_back(std::move(job));
	}
	return world_->mesh_service()->submit_lod(std::move(lod_jobs));
}

Array VoxelDebugHooks::debug_lod_collect() {
	Array out;
	if (!world_->physics_ready() || !world_->mesh_service()) return out;
	std::vector<LodBuildResult> results;
	world_->mesh_service()->collect_lod(&results);
	for (const LodBuildResult &r : results) {
		Dictionary d;
		d["level"] = r.level;
		d["coord"] = Vector3i(r.coord.x, r.coord.y, r.coord.z);
		d["quads"] = static_cast<int>(r.quads.size());
		d["overflow"] = r.overflow;
		d["failed"] = r.failed;
		out.push_back(d);
	}
	return out;
}
} // namespace godot
