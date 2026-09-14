#include "render/frame.h"
#include "core/world_store.h"
#include "lod/lod_grid.h"
#include "lod/lod_system.h"
#include "lod/lod_tree.h"
#include "render/beauty_camera.h"
#include "render/camera_params.h"
#include "render/composite_pass.h"
#include "render/contact_shadow_pass.h"
#include "render/deferred_pass.h"
#include "render/gbuffer.h"
#include "render/gpu_atlas.h"
#include "render/gpu_timings.h"
#include "render/grass_raster_pass.h"
#include "render/grass_scatter_pass.h"
#include "render/hiz_pass.h"
#include "render/inject_pass.h"
#include "render/island_atlas.h"
#include "render/island_cull_pass.h"
#include "render/lod_cull_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/material_atlas.h"
#include "render/orchestrator.h"
#include "render/outline_pass.h"
#include "render/raymarch_pass.h"
#include "render/ssao_pass.h"
#include "render/ssgi_pass.h"
#include "render/ssr_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/sun_ubo.h"
#include "render/world_streamer.h"
#include "shade/beauty_settings.h"
#include "shade/sun_cascades.h"
#include "world/residency.h"
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace godot;

VoxelFrame::VoxelFrame(RenderOrchestrator &render, LodSystem &lod, WorldStore &store,
		FrameHost &host) :
		render_(render), lod_(lod), store_(store), host_(host) {}

FrameRecord VoxelFrame::last_frame() const {
	std::lock_guard<std::mutex> lock(record_mutex_);
	return record_;
}

void VoxelFrame::note_fade_band(float fade_start, float fade_end) {
	std::lock_guard<std::mutex> lock(record_mutex_);
	record_.fade_start = fade_start;
	record_.fade_end = fade_end;
}

void VoxelFrame::note_lod_cull(bool two_phase, bool hiz_built, int first_pass_count) {
	std::lock_guard<std::mutex> lock(record_mutex_);
	record_.lod_two_phase = two_phase;
	record_.hiz_built = hiz_built;
	record_.lod_first_pass_count = first_pass_count;
}

// Was VoxelWorld::region_window().
ve::RegionWindow VoxelFrame::region_window() const {
	return store_.residency() ? store_.residency()->window() : ve::RegionWindow{};
}

// Was VoxelWorld::grass_reach_limit_m(); comment moved with it. Blades are scattered from
// resident BRICK data, so beyond the completely-resident radius every candidate is dropped.
// Before the streamer has run, complete_radius_m() is 0 and the configured radius is the
// honest answer.
float VoxelFrame::grass_reach_limit_m() const {
	float reach = store_.residency() ? store_.residency()->complete_radius_m() : 0.0f;
	if (reach <= 0.0f) reach = store_.config().residency_radius_m;
	return reach;
}

ve::GrassLayout VoxelFrame::grass_layout(const float cam_pos[3], const float view_proj[16]) const {
	// Grass is scattered from resident bricks, so a reach past the completely-resident
	// radius buys nothing: those candidates are dropped in stage 2 and the field ends on
	// a hard edge wherever residency happens to stop. Clamping here instead makes the
	// reach -- and therefore the ring fade that ends at it -- land on ground that exists.
	ve::GrassSettings gs = render_.grass_settings();
	gs.reach_m = std::min(gs.reach_m, grass_reach_limit_m());
	return ve::grass_layout(gs, cam_pos, view_proj);
}

// Was VoxelWorld::sun_ortho(); reads the sun live, as that method did.
ve::SunOrtho VoxelFrame::sun_ortho(int cascade) const {
	float cam[3];
	if (!lod_.last_camera(cam)) return ve::SunOrtho();
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(store_.config().stream_radius_m, SunShadowPass::kSize, c);
	if (n <= 0 || cascade < 0 || cascade >= n) return ve::SunOrtho();
	const ve::SunState sun = host_.frame_settings().sun;
	// A scene light hands over a basis that rotates continuously; a bare direction has to
	// have one derived, which is ill-conditioned near the zenith. Same choice as before.
	return sun.has_basis()
			? ve::sun_ortho_sphere(sun.dir, sun.right, sun.up, cam, c[cascade].radius,
					SunShadowPass::kSize)
			: ve::sun_ortho_sphere(sun.dir, cam, c[cascade].radius, SunShadowPass::kSize);
}

bool VoxelFrame::render_pre_opaque(RenderingDevice *rd, const FrameInputs &in) {
	const FrameSettings settings = host_.frame_settings();
	const bool near_field_enabled = settings.near_field_enabled;
	if (!rd) return false;
	const Vector2i size = in.size;
	if (size.x <= 0 || size.y <= 0) return false;
	GpuTimings *timings = render_.gpu_timings();
	timings->begin_frame(rd);
	auto abort_frame = [&]() { timings->abort_frame(); }; // invalidate all active markers

	const Transform3D cam = in.cam;
	const Projection proj = in.proj;
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
		return false; // ortho/degenerate
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
	const ve::RegionWindow win = region_window();
	cp.dims[0] = win.dim; cp.dims[1] = win.dim; cp.dims[2] = win.dim;
	cp.dims[3] = host_.island_slot_count();
	cp.region_origin[0] = win.origin.x;
	cp.region_origin[1] = win.origin.y;
	cp.region_origin[2] = win.origin.z;
	cp.region_origin[3] = 0; // The island-cull stage below sets the cull grid.
	const ve::IVec3 ab = store_.config().atlas_bricks;
	cp.atlas_bricks[0] = ab.x; cp.atlas_bricks[1] = ab.y; cp.atlas_bricks[2] = ab.z;
	const ve::BeautySettings beauty = render_.beauty_settings();
	const uint32_t beauty_flags = ve::pack_flags(beauty);
	std::memcpy(&cp.cam_pos[3], &beauty_flags, sizeof(float));

	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	const float cam_pos[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
	CameraUbo *ubo = render_.beauty_camera();
	if (!ubo || !ubo->ensure(rd)) {
		abort_frame();
		return false;
	}
	// Device-level operation: SSGI consumes this block before its compute list opens.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);
	const ve::SunState sun_state = settings.sun;
	if (SunUbo *sun_ubo = render_.sun_ubo()) {
		if (sun_ubo->ensure(rd)) sun_ubo->update(rd, sun_state);
	}

	// Volumes before anything that evaluates the field: an op naming a slot may already be
	// in the edit log, and the streamer is about to regenerate the bricks that read it.
	// Everything from here to the raymarch is world maintenance: volume uploads, the region
	// mark/free passes, and the indirect brick-generation dispatch. It was the only GPU work
	// in this callback with no timing label, and in the edit leg it is the largest single
	// contributor to a frame (M6 errata 3's 26.8 ms p99). Scope it before optimising it.
	timings->begin(rd, "stream");
	host_.drain_island_uploads(rd);
	WorldStreamer *st = host_.streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);
	timings->end(rd, "stream");
	// run_frame() recentres the toroidal region window. Refresh the already-built camera
	// push data for that published window; do not run streaming a second time just to obtain
	// constants that the existing run has already made current.
	{
		const ve::RegionWindow streamed_win = region_window();
		cp.dims[0] = streamed_win.dim;
		cp.dims[1] = streamed_win.dim;
		cp.dims[2] = streamed_win.dim;
		cp.dims[3] = host_.island_slot_count();
		cp.region_origin[0] = streamed_win.origin.x;
		cp.region_origin[1] = streamed_win.origin.y;
		cp.region_origin[2] = streamed_win.origin.z;
	}

	RaymarchPass *rmp = render_.raymarch_pass();
	GpuAtlas *atlas = render_.atlas();
	CompositePass *cmp = render_.composite_pass();
	MaterialAtlas *materials = render_.materials();
	GBuffer *gb = render_.gbuffer();
	DeferredPass *deferred = render_.deferred_pass();
	InjectPass *inject = render_.inject_pass();
	if (!rmp || !atlas || !cmp || !materials || !gb || !deferred || !inject) {
		abort_frame();
		return false;
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
	lod_.fade_band(&fade_start, &fade_end);
	note_fade_band(fade_start, fade_end);
	// The near field is never visible past the fade band's end: composite.frag.glsl's dither
	// threshold reaches 1.0 there, so every fragment beyond it is dropped and the far field
	// owns the pixel. Marching further was work whose result could not be used. Clamp the
	// reach to the seam the composite actually honours -- this costs nothing when the camera
	// looks down at close ground and saves the whole 80-200 m stretch when it looks at the
	// horizon, which is the case the move and ridge legs walk into.
	if (near_field_enabled) cp.params[2] = fade_end;
	if (!gb->ensure(rd, in.rsb, size)) {
		abort_frame();
		return false;
	}
	const float near_scale = settings.near_field_scale;
	const int rw = static_cast<int>(size.x * near_scale);
	const int rh = static_cast<int>(size.y * near_scale);
	if (rw <= 0 || rh <= 0) {
		abort_frame();
		return false;
	}
	const int islands = host_.island_slot_count();
	IslandCullPass *cull = render_.island_cull();
	RID mask;
	timings->begin(rd, "raymarch");
	if (cull && islands > 0 && cull->render(rd, *render_.islands(), cp, rw, rh, islands)) {
		mask = cull->mask_buffer();
		cp.region_origin[3] = cull->tiles_x();
		cp.atlas_bricks[3] = cull->tiles_y();
	}
	cp.dims[3] = islands;
	const RID effective_mask = mask.is_valid() ? mask : render_.islands()->fallback_mask();
	// If the island cull mask/target size changes, RaymarchPass releases its old target
	// textures. CompositePass owns a uniform set that references those textures, so drop that
	// dependent set first rather than later attempting to free a cascade-invalid RID.
	if (rmp->targets_need_rebuild(rw, rh, effective_mask)) {
		cmp->release_targets();
		cmp->invalidate_uniform_set(rd);
	}
	if (!rmp->render(rd, *atlas, render_.islands(), mask, cp, rw, rh, edit_state,
			render_.field_context())) {
		timings->cancel("raymarch");
		abort_frame();
		return false;
	}
	timings->end(rd, "raymarch");

	timings->begin(rd, "composite");
	cmp->draw(rd, *gb, rmp->albedo_texture(), rmp->surface_texture(), rmp->hitpos_texture(),
			view_proj, *materials, cp, fade_start, fade_end);
	if (!cmp->last_draw_ok()) {
		timings->cancel("composite");
		abort_frame();
		return false;
	}
	timings->end(rd, "composite");

	// Build HiZ from the near field's G-buffer depth before the LoD producer runs. The
	// deferred pass consumes both producers below, so neither field is shaded twice.
	// With the near field off there is no pre-LoD depth yet; skipping HiZ lets the LoD draw
	// every page instead of culling against an empty pyramid.
	HizPass *hiz = render_.hiz_pass();
	bool hiz_built = false;
	if (near_field_enabled && hiz) hiz_built = hiz->build(rd, gb->depth(), size);
	LodRasterPass *lod_raster = render_.lod_raster_pass();
	LodCullPass *lod_cull = render_.lod_cull_pass();
	SunShadowPass *sun = render_.sun_shadow_pass();
	ve::SunCascade cascades[ve::kSunCascades];
	const int cascade_count = ve::sun_cascades(store_.config().stream_radius_m,
			SunShadowPass::kSize, cascades);
	const bool clamp_levels = settings.sun_cascade_min_level;
	if (lod_.pool() && lod_raster && render_.materials()) {
		ve::LodCamera lod_cam;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				lod_cam.view_proj[c * 4 + r] = view_proj.columns[c][r];
		lod_cam.pos[0] = cam.origin.x;
		lod_cam.pos[1] = cam.origin.y;
		lod_cam.pos[2] = cam.origin.z;
		lod_cam.viewport[0] = size.x;
		lod_cam.viewport[1] = size.y;
		lod_.tick(lod_cam, hiz ? hiz->occlusion() : nullptr);
		const bool use_sun_shadow = sun && (beauty_flags & ve::kFlagSunMap) != 0u;
		auto build_sun_shadow = [&]() {
			if (!use_sun_shadow) return;
			timings->begin(rd, "sun_shadow");
			for (int i = 0; i < cascade_count; i++) {
				const ve::SunOrtho ortho = sun_ortho(i);
				// Ask BEFORE producing the cut: for cascade 2 the cut is the expensive
				// half, and it is skipped on most frames because a 3.9 m texel only
				// re-snaps every 3.9 m of travel.
				if (!sun->needs_rebuild(i, ortho)) continue;
				lod_.prepare_shadow_raster(cascades[i].radius,
						clamp_levels ? cascades[i].min_level : 0);
				sun->build(rd, *lod_.pool(), *lod_raster, i, ortho, false);
			}
			timings->end(rd, "sun_shadow");
			lod_.prepare_raster();
		};
		// Device-level indirect-argument uploads precede the cull list; draw() only opens
		// its own list after any cull list has ended. The first LoD occurrence is deliberately
		// before the sun-map build; the second follows it, so the parser never double-counts
		// shadow work as LoD work.
		const bool two_phase = lod_cull && lod_cull->is_valid() && hiz && hiz->pyramid().is_valid() &&
				hiz_built;
		note_lod_cull(two_phase, hiz_built,
				two_phase ? static_cast<int>(lod_cull->last_visible_pages().size()) : 0);
		if (!two_phase) {
			const std::vector<LodRasterPass::PageDraw> draw_pages = lod_raster->draw_pages();
			const bool split_for_shadow = use_sun_shadow && draw_pages.size() > 1;
			const size_t first_count = split_for_shadow ? (draw_pages.size() + 1) / 2 : draw_pages.size();
			std::vector<LodRasterPass::PageDraw> first_draw(draw_pages.begin(), draw_pages.begin() + first_count);
			std::vector<LodRasterPass::PageDraw> second_draw(draw_pages.begin() + first_count, draw_pages.end());
			if (!first_draw.empty()) {
				lod_.pool()->upload_draw_args(first_draw);
				timings->begin(rd, "lod");
				const bool first_lod_ok = lod_raster->draw(rd, *lod_.pool(), *materials, *gb,
						view_proj, cam_pos, static_cast<int>(first_draw.size()), fade_start, fade_end);
				if (!first_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return false; }
				timings->end(rd, "lod");
			}
			build_sun_shadow();
			if (!second_draw.empty()) {
				lod_.pool()->upload_draw_args(second_draw);
				timings->begin(rd, "lod");
				const bool second_lod_ok = lod_raster->draw(rd, *lod_.pool(), *materials, *gb,
						view_proj, cam_pos, static_cast<int>(second_draw.size()), fade_start, fade_end);
				if (!second_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return false; }
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
				lod_.pool()->upload_draw_args(first_pass_draw);
				timings->begin(rd, "lod");
				const bool first_lod_ok = lod_raster->draw(rd, *lod_.pool(), *materials, *gb,
						view_proj, cam_pos, first_pass_count, fade_start, fade_end);
				if (!first_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return false; }
				timings->end(rd, "lod");
				if (remaining_count > 0) hiz_built = hiz->build(rd, gb->depth(), size);
			}
			build_sun_shadow();
			if (remaining_count > 0) {
				lod_.pool()->upload_draw_args(remaining_draw);
				if (hiz_built) {
					lod_cull->set_first_pass_pages(first_pass_pages);
					lod_cull->run(rd, *lod_.pool(), hiz, view_proj, remaining_count,
							total_count, first_pass_count);
				} else {
					std::vector<int> visible = first_pass_pages;
					for (const LodRasterPass::PageDraw &pd : remaining_draw) visible.push_back(pd.page);
					lod_cull->set_last_visible_pages(visible);
				}
				timings->begin(rd, "lod");
				const bool remaining_lod_ok = lod_raster->draw(rd, *lod_.pool(), *materials, *gb,
						view_proj, cam_pos, remaining_count, fade_start, fade_end);
				if (!remaining_lod_ok) { timings->cancel("lod"); timings->abort_frame(); return false; }
				timings->end(rd, "lod");
			} else {
				lod_cull->set_last_visible_pages(first_pass_pages);
			}
		}
	}

	// Grass: one block, between the far field and the beauty stack. Blades write the same
	// G-buffer channels the far field writes, so everything below shades them unchanged.
	if (GrassScatterPass *grass = render_.grass_scatter_pass()) {
		timings->begin(rd, "grass");
		float grass_cam[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
		float grass_vp[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) grass_vp[c * 4 + r] = view_proj.columns[c][r];
		const ve::GrassLayout gl = grass_layout(grass_cam, grass_vp);
		GrassRasterPass *grass_raster = render_.grass_raster_pass();
		SunUbo *grass_sun = render_.sun_ubo();
		const bool grass_ok = grass_sun && grass->run(rd, *atlas, gl, region_window(),
				static_cast<float>(render_.beauty_frame()) / 60.0f, grass_sun->buffer()) &&
				grass_raster && grass_raster->draw(rd, *grass, *gb, view_proj, cam_pos);
		if (grass_ok) timings->end(rd, "grass");
		else timings->cancel("grass");
	}

	SsgiPass *ssgi = render_.ssgi_pass();
	if (ssgi) ssgi->clear_result();
	bool ssgi_ok = false;
	if (ssgi && beauty.ssgi) {
		timings->begin(rd, "ssgi");
		ssgi_ok = ssgi->render(rd, *gb, ubo->buffer(), render_.prev_view_proj(),
				render_.has_history(), beauty, render_.beauty_frame());
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
	// The old `static const float kNoSun[16]` fallback goes with the single-map path: the
	// UBO fill zeroes every cascade past `cascade_count` itself.
	const bool use_sun = sun && sun->is_valid() && sun->rebuilds(0) > 0 &&
			(beauty_flags & ve::kFlagSunMap) != 0u;
	dp.cascade_count = use_sun ? cascade_count : 0;
	for (int i = 0; i < cascade_count && use_sun; i++) {
		const float *vp = sun->view_proj(i);
		for (int k = 0; k < 16; k++) dp.sun_view_proj[i][k] = vp[k];
		dp.shadow_texel[i] = sun->texel_world(i);
		dp.shadow_depth_range_c[i] = sun->depth_range(i);
		dp.cascade_split[i] = cascades[i].radius;
	}
	// The map only shades what the LoD mesh drew; these are the distances the shader uses to
	// tell the two fields apart, and they are the same pair the composite and raster got.
	dp.fade_start = fade_start;
	dp.fade_end = fade_end;
	timings->begin(rd, "deferred");
	SsaoPass *ssao = render_.ssao_pass();
	if (ssao) ssao->clear_result();
	bool ssao_ok = false;
	if (ssao && (beauty_flags & ve::kFlagSsao) != 0u) {
		timings->begin(rd, "ssao");
		ssao_ok = ssao->render(rd, *gb, ubo->buffer(), beauty);
		if (ssao_ok) timings->end(rd, "ssao");
		else timings->cancel("ssao");
	}
	const bool deferred_ok = deferred->render(rd, *gb, *materials,
			ssgi_ok ? ssgi->result() : RID(), ssao_ok ? ssao->result() : RID(),
			use_sun ? sun->map() : RID(), dp);
	if (!deferred_ok) {
		timings->cancel("deferred");
		abort_frame();
		return false;
	}
	timings->end(rd, "deferred");
	timings->begin(rd, "inject");
	if (!inject->draw(rd, in.scene_color, in.scene_depth, gb->lit(), gb->depth())) {
		timings->cancel("inject");
		abort_frame();
		return false;
	}
	timings->end(rd, "inject");
	float current_view_proj[16];
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) current_view_proj[c * 4 + r] = view_proj.columns[c][r];
	render_.finish_beauty_frame(current_view_proj);
	return true;
}

bool VoxelFrame::render_post_opaque(RenderingDevice *rd, const FrameInputs &in) {
	if (!rd) return false;
	const Vector2i size = in.size;
	if (size.x <= 0 || size.y <= 0) return false;
	GpuTimings *timings = render_.gpu_timings();
	timings->poll(rd);

	const int normal_roughness_state = in.normal_roughness.is_valid() ? 1 : 0;
	const RID normal_rough = in.normal_roughness;
	// Task 9 found the texture reachable but constant/uncalibrated. Keep dynamic normal
	// creases disabled until a known-orientation calibration promotes the state to 2.
	const bool have_calibrated_normal_roughness = normal_roughness_state == 2;
	render_.set_normal_roughness_state(normal_roughness_state);

	const Transform3D cam = in.cam;
	const Projection proj = in.proj;
	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	const float cam_pos[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
	CameraUbo *ubo = render_.beauty_camera();
	if (!ubo || !ubo->ensure(rd)) return false;
	// Device-level operation: this precedes the contact-shadow compute list.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);

	const ve::BeautySettings settings = render_.beauty_settings();
	ContactShadowPass *cs = render_.contact_shadow_pass();
	if (cs) {
		timings->begin(rd, "contact");
		const bool contact_ok = cs->render(rd, in.scene_color, in.scene_depth, size,
				ubo->buffer(), settings);
		if (contact_ok) timings->end(rd, "contact");
		else timings->cancel("contact");
	}
	GBuffer *gb = render_.gbuffer();
	if (SsrPass *ssr = render_.ssr_pass()) {
		timings->begin(rd, "ssr");
		const bool ssr_ok = ssr->render(rd, in.scene_color, in.scene_depth,
				gb ? gb->surface() : RID(), gb ? gb->depth() : RID(), normal_rough,
				normal_roughness_state == 1, ubo->buffer(), size, settings);
		if (ssr_ok) timings->end(rd, "ssr");
		else timings->cancel("ssr");
	}
	if (OutlinePass *outline = render_.outline_pass(); outline && gb && gb->is_valid()) {
		timings->begin(rd, "outlines");
		const bool outline_ok = outline->render(rd, in.scene_color, in.scene_depth,
				gb->depth(), gb->surface(), normal_rough, have_calibrated_normal_roughness,
				ubo->buffer(), size, settings);
		if (outline_ok) timings->end(rd, "outlines");
		else timings->cancel("outlines");
	}
	// Non-visual copy: outline above is the last scene-colour mutation before glow/tonemap.
	if (gb && gb->is_valid()) {
		timings->begin(rd, "history");
		if (render_.downsample_history(rd, in.scene_color, *gb))
			timings->end(rd, "history");
		else timings->cancel("history");
	}
	timings->end_frame(rd);
	return true;
}
