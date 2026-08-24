#include "raymarch_compositor.h"
#include "voxel_world.h"
#include "render/composite_pass.h"
#include "render/deferred_pass.h"
#include "render/inject_pass.h"
#include "render/gbuffer.h"
#include "render/beauty_camera.h"
#include "render/ssgi_pass.h"
#include "render/ssao_pass.h"
#include "render/gpu_atlas.h"
#include "render/hiz_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/lod_cull_pass.h"
#include "lod/lod_tree.h"
#include "render/island_cull_pass.h"
#include "render/raymarch_pass.h"
#include "render/world_streamer.h"
#include "shade/beauty_settings.h"
#include "shade/cel.h"
#include "shade/sun_ortho.h"
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace godot;

RaymarchCompositor::RaymarchCompositor() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_PRE_OPAQUE);
}

void RaymarchCompositor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_path", "p"), &RaymarchCompositor::set_world_path);
	ClassDB::bind_method(D_METHOD("get_world_path"), &RaymarchCompositor::get_world_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "world_path"), "set_world_path", "get_world_path");
}

void RaymarchCompositor::_render_callback(int cb_type, RenderData *render_data) {
	if (cb_type != EFFECT_CALLBACK_TYPE_PRE_OPAQUE) return;
	if (world_path_.is_empty()) return;
	if (!render_data) return;
	VoxelWorld *world = nullptr;
	if (!voxel_try_begin_compositor_callback(world_path_, &world)) return;
	struct CallbackGuard {
		VoxelWorld *world;
		~CallbackGuard() { world->end_render_callback(); }
	} callback_guard{world};

	// Runs on the render thread (PRE_OPAQUE fires between the depth pre-pass and the opaque
	// pass, outside any engine draw list); the main RenderingDevice is safe to use here.
	// A requested shader reload is pumped before any pass pointer is read: it tears the GPU
	// objects down and rebuilds them here, so the rest of the callback runs against the new
	// pipelines. A failed pre-flight leaves the old pipelines untouched.
	world->pump_shader_reload();
	// ensure_initialized() is a no-op after the first frame.
	world->ensure_initialized();
	if (!world->is_initialized()) return;
	const bool near_field_enabled = world->get_effect_enabled("near_field");

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	RenderSceneBuffersRD *rsb = Object::cast_to<RenderSceneBuffersRD>(render_data->get_render_scene_buffers().ptr());
	RenderSceneData *sd = render_data->get_render_scene_data();
	if (!rd || !rsb || !sd) return;
	const Vector2i size = rsb->get_internal_size();
	if (size.x <= 0 || size.y <= 0) return;
	GpuTimings *timings = world->gpu_timings();
	timings->begin_frame(rd);
	auto abort_frame = [&]() { timings->abort_frame(); }; // invalidate all active markers

	const Transform3D cam = sd->get_cam_transform();
	const Projection proj = sd->get_cam_projection();
	// Deviation (documented): the engine's scene projection bakes in a y-flip (columns[1][1]
	// is negative on Godot 4.7 — empirically c11 < 0), and Projection::get_fov() returns the
	// HORIZONTAL fov, so the brief's tan_y = tan(fov/2) formula would be both sign- and
	// aspect-wrong. The half-angle tangents are the MAGNITUDES of the reciprocals of the
	// projection's diagonal (|1/c00| = tan(fov_x/2), |1/c11| = tan(fov_y/2)); the raymarch
	// shader already handles up/down via ndc.y, so the sign is discarded.
	const float tan_x = std::fabs(1.0f / static_cast<float>(proj.columns[0][0]));
	const float tan_y = std::fabs(1.0f / static_cast<float>(proj.columns[1][1]));
	if (!std::isfinite(tan_x) || !std::isfinite(tan_y) || tan_x <= 0.0f || tan_y <= 0.0f) {
		abort_frame();
		return; // ortho/degenerate
	}

	ve::CameraParams cp{};
	const Vector3 right = cam.basis.get_column(0);
	const Vector3 up = cam.basis.get_column(1);
	const Vector3 fwd = -cam.basis.get_column(2);
	cp.cam_pos[0] = cam.origin.x; cp.cam_pos[1] = cam.origin.y; cp.cam_pos[2] = cam.origin.z;
	cp.cam_right[0] = right.x; cp.cam_right[1] = right.y; cp.cam_right[2] = right.z;
	cp.cam_up[0] = up.x; cp.cam_up[1] = up.y; cp.cam_up[2] = up.z;
	cp.cam_fwd[0] = fwd.x; cp.cam_fwd[1] = fwd.y; cp.cam_fwd[2] = fwd.z;
	cp.params[0] = tan_x; cp.params[1] = tan_y;
	// Provisional reach; the real one is the fade band's end, read below once the streamer
	// has run. 0 = no near-field hits.
	cp.params[2] = near_field_enabled ? 200.0f : 0.0f;
	const Vector3i sr = world->get_world_size_regions();
	cp.dims[0] = sr.x; cp.dims[1] = sr.y; cp.dims[2] = sr.z;
	cp.dims[3] = world->island_slot_count();
	const Vector3i ob = world->get_world_origin_bricks();
	cp.region_origin[0] = ob.x / 32; cp.region_origin[1] = ob.y / 32;
	cp.region_origin[2] = ob.z / 32;
	cp.region_origin[3] = 0; // Task 11 sets the cull grid
	const Vector3i ab = world->get_atlas_bricks();
	cp.atlas_bricks[0] = ab.x; cp.atlas_bricks[1] = ab.y; cp.atlas_bricks[2] = ab.z;
	const ve::BeautySettings beauty = world->beauty_settings();
	const uint32_t beauty_flags = ve::pack_flags(beauty);
	std::memcpy(&cp.cam_pos[3], &beauty_flags, sizeof(float));

	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	const float cam_pos[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
	CameraUbo *ubo = world->beauty_camera();
	if (!ubo || !ubo->ensure(rd)) {
		abort_frame();
		return;
	}
	// Device-level operation: SSGI consumes this block before its compute list opens.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);

	// Volumes before anything that evaluates the field: an op naming a slot may already be
	// in the edit log, and the streamer is about to regenerate the bricks that read it.
	// Everything from here to the raymarch is world maintenance: volume uploads, the region
	// mark/free passes, and the indirect brick-generation dispatch. It was the only GPU work
	// in this callback with no timing label, and in the edit leg it is the largest single
	// contributor to a frame (M6 errata 3's 26.8 ms p99). Scope it before optimising it.
	timings->begin(rd, "stream");
	world->drain_island_uploads(rd);
	WorldStreamer *st = world->streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);
	timings->end(rd, "stream");

	RaymarchPass *rmp = world->raymarch_pass();
	GpuAtlas *atlas = world->atlas();
	CompositePass *cmp = world->composite_pass();
	MaterialAtlas *materials = world->material_atlas();
	GBuffer *gb = world->gbuffer();
	DeferredPass *deferred = world->deferred_pass();
	InjectPass *inject = world->inject_pass();
	if (!rmp || !atlas || !cmp || !materials || !gb || !deferred || !inject) {
		abort_frame();
		return;
	}
	float edit_state[6] = {0, 0, 0, 0, 0, 0};
	if (st && st->last_edit_radius() > 0.0f) {
		edit_state[0] = st->last_edit_center()[0];
		edit_state[1] = st->last_edit_center()[1];
		edit_state[2] = st->last_edit_center()[2];
		edit_state[3] = st->last_edit_radius();
		edit_state[4] = static_cast<float>(st->last_edit_type());
		edit_state[5] = static_cast<float>(st->last_edit_material());
	}

	// Master-API note: rsb->get_color_texture()/get_depth_texture() exist on godot-cpp master
	// (render_scene_buffers_rd.hpp) and return the non-MSAA internal color/depth textures —
	// the same RIDs the engine's own framebuffers use when MSAA is disabled (verified against
	// render_forward_clustered.cpp), so the composite writes into the actual scene buffers.
	// Both fields fade at the SAME two distances, and those distances follow how far the
	// near field's bricks actually reach this frame -- not the spec's 120/150, which assumes
	// an atlas three times this one. Read once here so the composite and every far-field
	// draw below cannot disagree within a frame.
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	world->lod_fade_band(&fade_start, &fade_end);
	// The near field is never visible past the fade band's end: composite.frag.glsl's dither
	// threshold reaches 1.0 there, so every fragment beyond it is dropped and the far field
	// owns the pixel. Marching further was work whose result could not be used. Clamp the
	// reach to the seam the composite actually honours -- this costs nothing when the camera
	// looks down at close ground and saves the whole 80-200 m stretch when it looks at the
	// horizon, which is the case the move and ridge legs walk into.
	if (near_field_enabled) cp.params[2] = fade_end;
	if (!gb->ensure(rd, rsb, size)) {
		abort_frame();
		return;
	}
	const float near_scale = world->get_near_field_scale();
	const int rw = static_cast<int>(size.x * near_scale);
	const int rh = static_cast<int>(size.y * near_scale);
	if (rw <= 0 || rh <= 0) {
		abort_frame();
		return;
	}
	const int islands = world->island_slot_count();
	IslandCullPass *cull = world->island_cull();
	RID mask;
	timings->begin(rd, "raymarch");
	if (cull && islands > 0 && cull->render(rd, *world->islands(), cp, rw, rh, islands)) {
		mask = cull->mask_buffer();
		cp.region_origin[3] = cull->tiles_x();
		cp.atlas_bricks[3] = cull->tiles_y();
	}
	cp.dims[3] = islands;
	const RID effective_mask = mask.is_valid() ? mask : world->islands()->fallback_mask();
	// If the island cull mask/target size changes, RaymarchPass releases its old target
	// textures. CompositePass owns a uniform set that references those textures, so drop that
	// dependent set first rather than later attempting to free a cascade-invalid RID.
	if (rmp->targets_need_rebuild(rw, rh, effective_mask)) {
		cmp->release_targets();
		cmp->invalidate_uniform_set(rd);
	}
	if (!rmp->render(rd, *atlas, world->islands(), mask, cp, rw, rh, edit_state)) {
		timings->cancel("raymarch");
		abort_frame();
		return;
	}
	timings->end(rd, "raymarch");

	timings->begin(rd, "composite");
	cmp->draw(rd, *gb, rmp->albedo_texture(), rmp->surface_texture(), rmp->hitpos_texture(),
			view_proj, *materials, cam_pos, fade_start, fade_end);
	if (!cmp->last_draw_ok()) {
		timings->cancel("composite");
		abort_frame();
		return;
	}
	timings->end(rd, "composite");

	// Build HiZ from the near field's G-buffer depth before the LoD producer runs. The
	// deferred pass consumes both producers below, so neither field is shaded twice.
	// With the near field off there is no pre-LoD depth yet; skipping HiZ lets the LoD draw
	// every page instead of culling against an empty pyramid.
	HizPass *hiz = world->hiz_pass();
	bool hiz_built = false;
	if (near_field_enabled && hiz) hiz_built = hiz->build(rd, gb->depth(), size);
	LodRasterPass *lod_raster = world->lod_raster_pass();
	LodCullPass *lod_cull = world->lod_cull_pass();
	SunShadowPass *sun = world->sun_shadow_pass();
	if (world->lod_pool() && lod_raster && world->material_atlas()) {
		ve::LodCamera lod_cam;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				lod_cam.view_proj[c * 4 + r] = view_proj.columns[c][r];
		lod_cam.pos[0] = cam.origin.x;
		lod_cam.pos[1] = cam.origin.y;
		lod_cam.pos[2] = cam.origin.z;
		lod_cam.viewport[0] = size.x;
		lod_cam.viewport[1] = size.y;
		world->lod_tick(lod_cam, hiz ? hiz->occlusion() : nullptr);
		const bool use_sun_shadow = sun && (beauty_flags & ve::kFlagSunMap) != 0u;
		auto build_sun_shadow = [&]() {
			if (!use_sun_shadow) return;
			world->prepare_lod_shadow_raster();
			timings->begin(rd, "sun_shadow");
			const ve::WorldBounds wb = world->world_bounds();
			float lo[3];
			float hi[3];
			wb.aabb(lo, hi);
			const bool shadow_ok = sun->build(rd, *world->lod_pool(), *lod_raster,
					ve::sun_ortho(ve::kSunDir, lo, hi, SunShadowPass::kSize), false);
			if (shadow_ok) timings->end(rd, "sun_shadow");
			else timings->cancel("sun_shadow");
			world->prepare_lod_raster();
		};
		// Device-level indirect-argument uploads precede the cull list; draw() only opens
		// its own list after any cull list has ended. The first LoD occurrence is deliberately
		// before the sun-map build; the second follows it, so the parser never double-counts
		// shadow work as LoD work.
		const bool two_phase = lod_cull && lod_cull->is_valid() && hiz && hiz->pyramid().is_valid() &&
				hiz_built;
		if (!two_phase) {
			const std::vector<LodRasterPass::PageDraw> draw_pages = lod_raster->draw_pages();
			const bool split_for_shadow = use_sun_shadow && draw_pages.size() > 1;
			const size_t first_count = split_for_shadow ? (draw_pages.size() + 1) / 2 : draw_pages.size();
			std::vector<LodRasterPass::PageDraw> first_draw(draw_pages.begin(), draw_pages.begin() + first_count);
			std::vector<LodRasterPass::PageDraw> second_draw(draw_pages.begin() + first_count, draw_pages.end());
			if (!first_draw.empty()) {
				world->lod_pool()->upload_draw_args(first_draw);
				timings->begin(rd, "lod");
				const bool first_lod_ok = lod_raster->draw(rd, *world->lod_pool(), *materials, *gb,
						view_proj, cam_pos, static_cast<int>(first_draw.size()), fade_start, fade_end);
				if (!first_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return; }
				timings->end(rd, "lod");
			}
			build_sun_shadow();
			if (!second_draw.empty()) {
				world->lod_pool()->upload_draw_args(second_draw);
				timings->begin(rd, "lod");
				const bool second_lod_ok = lod_raster->draw(rd, *world->lod_pool(), *materials, *gb,
						view_proj, cam_pos, static_cast<int>(second_draw.size()), fade_start, fade_end);
				if (!second_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return; }
				timings->end(rd, "lod");
			}
		} else {
			// Draw the previous visible set, then place sun shadow between it and the culled
			// remainder. A failed HiZ rebuild still falls back to drawing all remaining pages.
			const std::vector<LodRasterPass::PageDraw> &draw_pages = lod_raster->draw_pages();
			const std::vector<int> &last_visible = lod_cull->last_visible_pages();
			std::vector<LodRasterPass::PageDraw> first_pass_draw;
			std::vector<LodRasterPass::PageDraw> remaining_draw;
			first_pass_draw.reserve(draw_pages.size());
			remaining_draw.reserve(draw_pages.size());
			for (const LodRasterPass::PageDraw &pd : draw_pages) {
				if (std::binary_search(last_visible.begin(), last_visible.end(), pd.page))
					first_pass_draw.push_back(pd);
				else
					remaining_draw.push_back(pd);
			}
			std::vector<int> first_pass_pages;
			first_pass_pages.reserve(first_pass_draw.size());
			for (const LodRasterPass::PageDraw &pd : first_pass_draw) first_pass_pages.push_back(pd.page);
			const int first_pass_count = static_cast<int>(first_pass_draw.size());
			const int remaining_count = static_cast<int>(remaining_draw.size());
			const int total_count = lod_raster->draw_page_count();
			if (first_pass_count > 0) {
				world->lod_pool()->upload_draw_args(first_pass_draw);
				timings->begin(rd, "lod");
				const bool first_lod_ok = lod_raster->draw(rd, *world->lod_pool(), *materials, *gb,
						view_proj, cam_pos, first_pass_count, fade_start, fade_end);
				if (!first_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return; }
				timings->end(rd, "lod");
				if (remaining_count > 0) hiz_built = hiz->build(rd, gb->depth(), size);
			}
			build_sun_shadow();
			if (remaining_count > 0) {
				world->lod_pool()->upload_draw_args(remaining_draw);
				if (hiz_built) {
					lod_cull->set_first_pass_pages(first_pass_pages);
					lod_cull->run(rd, *world->lod_pool(), hiz, view_proj, remaining_count,
							total_count, first_pass_count);
				} else {
					std::vector<int> visible = first_pass_pages;
					for (const LodRasterPass::PageDraw &pd : remaining_draw) visible.push_back(pd.page);
					lod_cull->set_last_visible_pages(visible);
				}
				timings->begin(rd, "lod");
				const bool remaining_lod_ok = lod_raster->draw(rd, *world->lod_pool(), *materials, *gb,
						view_proj, cam_pos, remaining_count, fade_start, fade_end);
				if (!remaining_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return; }
				timings->end(rd, "lod");
			} else {
				lod_cull->set_last_visible_pages(first_pass_pages);
			}
		}
	}

	SsgiPass *ssgi = world->ssgi_pass();
	if (ssgi) ssgi->clear_result();
	bool ssgi_ok = false;
	if (ssgi && beauty.ssgi) {
		timings->begin(rd, "ssgi");
		ssgi_ok = ssgi->render(rd, *gb, ubo->buffer(), world->prev_view_proj(),
				world->has_history(), beauty, world->beauty_frame());
		if (ssgi_ok) timings->end(rd, "ssgi");
		else timings->cancel("ssgi");
	}

	DeferredPass::Params dp;
	const Projection inv = view_proj.inverse();
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			dp.inv_view_proj[c * 4 + r] = inv.columns[c][r];
	dp.cam_pos[0] = cam.origin.x;
	dp.cam_pos[1] = cam.origin.y;
	dp.cam_pos[2] = cam.origin.z;
	dp.flags = beauty_flags;
	static const float kNoSun[16] = {};
	const bool use_sun = sun && sun->is_valid() && sun->rebuilds() > 0 &&
			(beauty_flags & ve::kFlagSunMap) != 0u;
	timings->begin(rd, "deferred");
	SsaoPass *ssao = world->ssao_pass();
	if (ssao) ssao->clear_result();
	bool ssao_ok = false;
	if (ssao && (beauty_flags & ve::kFlagSsao) != 0u) {
		timings->begin(rd, "ssao");
		ssao_ok = ssao->render(rd, *gb, ubo->buffer(), beauty);
		if (ssao_ok) timings->end(rd, "ssao");
		else timings->cancel("ssao");
	}
	const bool deferred_ok = deferred->render(rd, *gb, *materials, ssgi_ok ? ssgi->result() : RID(),
			ssao_ok ? ssao->result() : RID(),
			use_sun ? sun->map() : RID(), use_sun ? sun->view_proj() : kNoSun,
			use_sun ? sun->texel_world() : 0.0f, dp);
	if (!deferred_ok) {
		timings->cancel("deferred");
		abort_frame();
		return;
	}
	timings->end(rd, "deferred");
	timings->begin(rd, "inject");
	if (!inject->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(), gb->lit(), gb->depth())) {
		timings->cancel("inject");
		abort_frame();
		return;
	}
	timings->end(rd, "inject");
	float current_view_proj[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) current_view_proj[c * 4 + r] = view_proj.columns[c][r];
	world->finish_beauty_frame(current_view_proj);

}
