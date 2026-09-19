#include "render/frame.h"
#include "render/frame_params.h"
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
#include "render/leaf_scatter_pass.h"
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

VoxelFrame::VoxelFrame(RenderOrchestrator &render, LodSystem &lod, WorldStore &store) :
		render_(render), lod_(lod), store_(store) {}

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
	// Near grass covers exactly what the RAYMARCHER covers, and the far LoD rings own
	// everything past it. Two limits say where that is, and the nearer one wins:
	//
	//  - the completely-resident radius, because blades are scattered from resident bricks and
	//    a reach past it buys nothing: those candidates are dropped in stage 2 and the field
	//    ends on a hard edge wherever residency happens to stop;
	//  - the fade band's end, the seam where composite.frag.glsl stops drawing the near field
	//    at all. Grass past it would stand on pixels the far field owns, which is the LoD
	//    rings' job -- and they are scattered against the field, so they see terrain the brick
	//    atlas no longer holds.
	//
	// GrassSettings::reach_m is the cap over both, not the shipped reach: its default sits
	// above every seam the band can produce, so the raymarched area is covered whatever the
	// streamer managed this frame.
	ve::GrassSettings gs = render_.grass_settings();
	float fade_start = ve::kLodFadeStartM;
	float fade_end = ve::kLodFadeEndM;
	lod_.fade_band(&fade_start, &fade_end);
	gs.reach_m = std::min(gs.reach_m, std::min(fade_end, grass_reach_limit_m()));
	return ve::grass_layout(gs, cam_pos, view_proj);
}

ve::LeafLayout VoxelFrame::leaf_layout(const float cam_pos[3], const float view_proj[16]) const {
	// No fade-band/residency clamp: stage 1 attaches every candidate to a live bark voxel in
	// the atlas, so residency bounds the list by itself. See the declaration in frame.h.
	return ve::leaf_layout(render_.leaf_settings(), cam_pos, view_proj);
}

// Was VoxelWorld::sun_ortho(); reads the sun live, as that method did.
ve::SunOrtho VoxelFrame::sun_ortho(int cascade) const {
	float cam[3];
	if (!lod_.last_camera(cam)) return ve::SunOrtho();
	ve::SunCascade c[ve::kSunCascades];
	const int n = ve::sun_cascades(store_.config().stream_radius_m, SunShadowPass::kSize, c);
	if (n <= 0 || cascade < 0 || cascade >= n) return ve::SunOrtho();
	const ve::SunState sun = render_.frame_settings().sun;
	// A scene light hands over a basis that rotates continuously; a bare direction has to
	// have one derived, which is ill-conditioned near the zenith. Same choice as before.
	return sun.has_basis()
			? ve::sun_ortho_sphere(sun.dir, sun.right, sun.up, cam, c[cascade].radius,
					SunShadowPass::kSize)
			: ve::sun_ortho_sphere(sun.dir, cam, c[cascade].radius, SunShadowPass::kSize);
}

bool VoxelFrame::render_pre_opaque(RenderingDevice *rd, const FrameInputs &in) {
	reset_stages();
	const FrameSettings settings = render_.frame_settings();
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
	const ve::BeautySettings beauty = render_.beauty_settings();
	const uint32_t beauty_flags = ve::pack_flags(beauty);
	ve::set_near_field_flags(&cp, beauty_flags);

	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	const float cam_pos[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
	CameraUbo *ubo = render_.passes().beauty_camera;
	if (!ubo || !ubo->ensure(rd)) {
		abort_frame();
		return false;
	}
	// Device-level operation: SSGI consumes this block before its compute list opens.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);
	const ve::SunState sun_state = settings.sun;
	if (SunUbo *sun_ubo = render_.passes().sun_ubo) {
		if (sun_ubo->ensure(rd)) sun_ubo->update(rd, sun_state);
	}

	// Volumes before anything that evaluates the field: an op naming a slot may already be
	// in the edit log, and the streamer is about to regenerate the bricks that read it.
	// Everything from here to the raymarch is world maintenance: volume uploads, the region
	// mark/free passes, and the indirect brick-generation dispatch. It was the only GPU work
	// in this callback with no timing label, and in the edit leg it is the largest single
	// contributor to a frame (M6 errata 3's 26.8 ms p99). Scope it before optimising it.
	timings->begin(rd, "stream");
	render_.drain_island_uploads(rd);
	WorldStreamer *st = render_.streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);
	end_stage(rd, kStageStream);
	// run_frame() recentres the toroidal region window. Refresh the already-built camera
	// push data for that published window; do not run streaming a second time just to obtain
	// constants that the existing run has already made current.
	// run_frame() recentred the toroidal region window; the world half of the push block is
	// filled from the window it published. cull tiles (region_origin.w / atlas_bricks.w)
	// are set by the island cull below.
	ve::set_near_field_world(&cp, store_.region_window(), render_.island_slot_count(),
			store_.config().atlas_bricks);
	cp.region_origin[3] = 0;

	RaymarchPass *rmp = render_.passes().raymarch;
	GpuAtlas *atlas = render_.passes().atlas;
	CompositePass *cmp = render_.passes().composite;
	MaterialAtlas *materials = render_.passes().materials;
	GBuffer *gb = render_.passes().gbuffer;
	DeferredPass *deferred = render_.passes().deferred;
	InjectPass *inject = render_.passes().inject;
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
	const int islands = render_.island_slot_count();
	IslandCullPass *cull = render_.passes().island_cull;
	RID mask;
	timings->begin(rd, "raymarch");
	if (cull && islands > 0 && cull->render(rd, *render_.passes().islands, cp, rw, rh, islands)) {
		mask = cull->mask_buffer();
		cp.region_origin[3] = cull->tiles_x();
		cp.atlas_bricks[3] = cull->tiles_y();
	}
	cp.dims[3] = islands;
	const RID effective_mask = mask.is_valid() ? mask : render_.passes().islands->fallback_mask();
	// If the island cull mask/target size changes, RaymarchPass releases its old target
	// textures. CompositePass's uniform set binds them and rebuilds itself on the new RIDs;
	// its framebuffer is dropped here as it always was.
	if (rmp->targets_need_rebuild(rw, rh, effective_mask)) cmp->release_targets();
	if (!rmp->render(rd, *atlas, render_.passes().islands, mask, cp, rw, rh, edit_state,
			render_.passes().field_context)) {
		cancel_stage(kStageRaymarch);
		abort_frame();
		return false;
	}
	end_stage(rd, kStageRaymarch);

	timings->begin(rd, "composite");
	cmp->draw(rd, *gb, rmp->albedo_texture(), rmp->surface_texture(), rmp->hitpos_texture(),
			view_proj, *materials, cp, fade_start, fade_end, in.debug.marker);
	if (!cmp->last_draw_ok()) {
		cancel_stage(kStageComposite);
		abort_frame();
		return false;
	}
	end_stage(rd, kStageComposite);

	// Build HiZ from the near field's G-buffer depth before the LoD producer runs. The
	// deferred pass consumes both producers below, so neither field is shaded twice.
	// With the near field off there is no pre-LoD depth yet; skipping HiZ lets the LoD draw
	// every page instead of culling against an empty pyramid.
	HizPass *hiz = render_.passes().hiz;
	bool hiz_built = false;
	if (near_field_enabled && hiz) hiz_built = hiz->build(rd, gb->depth(), size);
	LodRasterPass *lod_raster = render_.passes().lod_raster;
	LodCullPass *lod_cull = render_.passes().lod_cull;
	SunShadowPass *sun = render_.passes().sun_shadow;
	ve::SunCascade cascades[ve::kSunCascades];
	const int cascade_count = ve::sun_cascades(store_.config().stream_radius_m,
			SunShadowPass::kSize, cascades);
	const bool clamp_levels = settings.sun_cascade_min_level;
	if (!in.debug.skip_far_field && lod_.pool() && lod_raster && render_.passes().materials) {
		ve::LodCamera lod_cam;
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++)
				lod_cam.view_proj[c * 4 + r] = view_proj.columns[c][r];
		lod_cam.pos[0] = cam.origin.x;
		lod_cam.pos[1] = cam.origin.y;
		lod_cam.pos[2] = cam.origin.z;
		lod_cam.viewport[0] = in.debug.lod_viewport.x > 0 ? in.debug.lod_viewport.x : size.x;
		lod_cam.viewport[1] = in.debug.lod_viewport.y > 0 ? in.debug.lod_viewport.y : size.y;
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
			end_stage(rd, kStageSunShadow);
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
						view_proj, cam_pos, static_cast<int>(first_draw.size()), fade_start, fade_end, in.debug.marker);
				if (!first_lod_ok) { cancel_stage(kStageLod); timings->abort_frame(); return false; }
				end_stage(rd, kStageLod);
			}
			build_sun_shadow();
			if (!second_draw.empty()) {
				lod_.pool()->upload_draw_args(second_draw);
				timings->begin(rd, "lod");
				const bool second_lod_ok = lod_raster->draw(rd, *lod_.pool(), *materials, *gb,
						view_proj, cam_pos, static_cast<int>(second_draw.size()), fade_start, fade_end, in.debug.marker);
				if (!second_lod_ok) { cancel_stage(kStageLod); timings->abort_frame(); return false; }
				end_stage(rd, kStageLod);
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
						view_proj, cam_pos, first_pass_count, fade_start, fade_end, in.debug.marker);
				if (!first_lod_ok) { cancel_stage(kStageLod); timings->abort_frame(); return false; }
				end_stage(rd, kStageLod);
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
						view_proj, cam_pos, remaining_count, fade_start, fade_end, in.debug.marker);
				if (!remaining_lod_ok) { cancel_stage(kStageLod); timings->abort_frame(); return false; }
				end_stage(rd, kStageLod);
			} else {
				lod_cull->set_last_visible_pages(first_pass_pages);
			}
		}
	}

	// Grass: one block, between the far field and the beauty stack. Blades write the same
	// G-buffer channels the far field writes, so everything below shades them unchanged.
	if (GrassScatterPass *grass = render_.passes().grass_scatter) {
		timings->begin(rd, "grass");
		float grass_cam[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
		float grass_vp[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) grass_vp[c * 4 + r] = view_proj.columns[c][r];
		const ve::GrassLayout gl = grass_layout(grass_cam, grass_vp);
		GrassRasterPass *grass_raster = render_.passes().grass_raster;
		SunUbo *grass_sun = render_.passes().sun_ubo;
		const bool grass_ok = grass_sun && grass->run(rd, *atlas, gl, store_.region_window(),
				static_cast<float>(render_.beauty_frame()) / 60.0f, grass_sun->buffer(),
				render_.passes().field_context) &&
				grass_raster && grass_raster->draw(rd, *grass, *gb, view_proj, cam_pos);
		if (grass_ok) end_stage(rd, kStageGrass);
		else cancel_stage(kStageGrass);
	}

	// Leaves: the tree cull, right beside the grass it shadows. Nothing draws yet -- Task 12
	// hands this buffer list to the raster -- but the pass runs on the real window every
	// frame, so the chop contract holds whether or not anyone is looking.
	if (LeafScatterPass *leaf = render_.passes().leaf_scatter) {
		timings->begin(rd, "leaves");
		float leaf_cam[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
		float leaf_vp[16];
		for (int c = 0; c < 4; c++)
			for (int r = 0; r < 4; r++) leaf_vp[c * 4 + r] = view_proj.columns[c][r];
		const ve::LeafLayout ll = leaf_layout(leaf_cam, leaf_vp);
		SunUbo *leaf_sun = render_.passes().sun_ubo;
		// Stage 1 never reads the SunUbo (Task 11's march will); an absent one is no reason
		// to skip the cull, so an empty RID travels through unbound.
		const bool leaf_ok = leaf->run(rd, *atlas, ll, store_.region_window(),
				static_cast<float>(render_.beauty_frame()) / 60.0f,
				leaf_sun ? leaf_sun->buffer() : RID(), render_.passes().field_context);
		if (leaf_ok) timings->end(rd, "leaves");
		else timings->cancel("leaves");
	}

	SsgiPass *ssgi = render_.passes().ssgi;
	if (ssgi) ssgi->clear_result();
	bool ssgi_ok = false;
	if (ssgi && beauty.ssgi) {
		timings->begin(rd, "ssgi");
		ssgi_ok = ssgi->render(rd, *gb, ubo->buffer(), render_.prev_view_proj(),
				render_.has_history(), beauty, render_.beauty_frame());
		if (ssgi_ok) end_stage(rd, kStageSsgi);
		else cancel_stage(kStageSsgi);
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
	for (int k = 0; k < 3; k++) dp.ambient[k] = beauty.ambient[k];
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
	dp.probe_mode = in.debug.deferred_view;
	SsaoPass *ssao = render_.passes().ssao;
	if (ssao) ssao->clear_result();
	bool ssao_ok = false;
	if (ssao && (beauty_flags & ve::kFlagSsao) != 0u) {
		timings->begin(rd, "ssao");
		ssao_ok = ssao->render(rd, *gb, ubo->buffer(), beauty);
		if (ssao_ok) end_stage(rd, kStageSsao);
		else cancel_stage(kStageSsao);
	}
	// S8: open "deferred" only after SSAO has closed, so the label times lighting alone.
	timings->begin(rd, "deferred");
	const bool deferred_ok = deferred->render(rd, *gb, *materials,
			ssgi_ok ? ssgi->result() : RID(), ssao_ok ? ssao->result() : RID(),
			use_sun ? sun->map() : RID(), dp);
	if (!deferred_ok) {
		cancel_stage(kStageDeferred);
		abort_frame();
		return false;
	}
	end_stage(rd, kStageDeferred);
	timings->begin(rd, "inject");
	if (!inject->draw(rd, in.scene_color, in.scene_depth, gb->lit(), gb->depth())) {
		cancel_stage(kStageInject);
		abort_frame();
		return false;
	}
	end_stage(rd, kStageInject);
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
	CameraUbo *ubo = render_.passes().beauty_camera;
	if (!ubo || !ubo->ensure(rd)) return false;
	// Device-level operation: this precedes the contact-shadow compute list.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);

	const ve::BeautySettings settings = render_.beauty_settings();
	ContactShadowPass *cs = render_.passes().contact_shadow;
	if (cs) {
		timings->begin(rd, "contact");
		const bool contact_ok = cs->render(rd, in.scene_color, in.scene_depth, size,
				ubo->buffer(), settings);
		if (contact_ok) end_stage(rd, kStageContact);
		else cancel_stage(kStageContact);
	}
	GBuffer *gb = render_.passes().gbuffer;
	if (SsrPass *ssr = render_.passes().ssr) {
		timings->begin(rd, "ssr");
		const bool ssr_ok = ssr->render(rd, in.scene_color, in.scene_depth,
				gb ? gb->surface() : RID(), gb ? gb->depth() : RID(), normal_rough,
				normal_roughness_state == 1, ubo->buffer(), size, settings);
		if (ssr_ok) end_stage(rd, kStageSsr);
		else cancel_stage(kStageSsr);
	}
	if (OutlinePass *outline = render_.passes().outline; outline && gb && gb->is_valid()) {
		timings->begin(rd, "outlines");
		const bool outline_ok = outline->render(rd, in.scene_color, in.scene_depth,
				gb->depth(), gb->surface(), normal_rough, have_calibrated_normal_roughness,
				ubo->buffer(), size, settings);
		if (outline_ok) end_stage(rd, kStageOutlines);
		else cancel_stage(kStageOutlines);
	}
	// Non-visual copy: outline above is the last scene-colour mutation before glow/tonemap.
	if (gb && gb->is_valid()) {
		timings->begin(rd, "history");
		if (render_.downsample_history(rd, in.scene_color, *gb))
			end_stage(rd, kStageHistory);
		else cancel_stage(kStageHistory);
	}
	timings->end_frame(rd);
	return true;
}

const char *godot::frame_stage_name(FrameStage stage) {
	static const char *const kNames[kStageCount] = {"stream", "raymarch", "composite", "lod",
			"sun_shadow", "grass", "ssgi", "deferred", "ssao", "inject", "contact", "ssr",
			"outlines", "history"};
	return stage < kStageCount ? kNames[stage] : "";
}

void VoxelFrame::end_stage(RenderingDevice *rd, FrameStage stage) {
	render_.gpu_timings()->end(rd, frame_stage_name(stage));
	std::lock_guard<std::mutex> lock(record_mutex_);
	record_.stages_ok |= 1u << stage;
}

void VoxelFrame::cancel_stage(FrameStage stage) {
	render_.gpu_timings()->cancel(frame_stage_name(stage));
	std::lock_guard<std::mutex> lock(record_mutex_);
	record_.stages_cancelled |= 1u << stage;
}

void VoxelFrame::reset_stages() {
	std::lock_guard<std::mutex> lock(record_mutex_);
	record_.stages_ok = 0;
	record_.stages_cancelled = 0;
}

FrameInputs VoxelFrame::looking_at(Vector3 pos, Vector3 fwd, int w, int h, float fov_y_rad,
		float z_near, float z_far) {
	FrameInputs in;
	in.size = Vector2i(w, h);
	if (w <= 0 || h <= 0) return in;
	const float p[3] = {pos.x, pos.y, pos.z};
	const float f[3] = {fwd.x, fwd.y, fwd.z};
	const ve::ProbeCamera pc = ve::probe_camera(p, f, w, h, fov_y_rad, z_near, z_far);
	Basis basis;
	basis.set_column(0, Vector3(pc.right[0], pc.right[1], pc.right[2]));
	basis.set_column(1, Vector3(pc.up[0], pc.up[1], pc.up[2]));
	basis.set_column(2, -Vector3(pc.fwd[0], pc.fwd[1], pc.fwd[2]));
	in.cam = Transform3D(basis, pos);
	Projection view_proj;
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++) view_proj.columns[c][r] = pc.lod.view_proj[c * 4 + r];
	// view_proj = proj * view and view = cam^-1, so proj = view_proj * cam.
	in.proj = view_proj * Projection(in.cam);
	return in;
}

FrameInputs VoxelFrame::prepare_headless(RenderingDevice *rd, const FrameInputs &in) {
	FrameInputs out = in;
	out.rsb = nullptr;
	out.normal_roughness = RID();
	out.scene_color = RID();
	out.scene_depth = RID();
	if (!rd || rd != render_.local_rd() || in.size.x <= 0 || in.size.y <= 0) return out;
	const GBuffer *gb = render_.passes().gbuffer;
	if (headless_.size() != in.size || (gb && gb->size() != in.size)) {
		// The G-buffer and the scene targets are about to be reallocated. These passes cache a
		// framebuffer over them and expose release_targets() for exactly this -- the same drops
		// the probes made by hand. Uniform sets keyed by RID rebuild themselves. Never tear down
		// DeferredPass/ContactShadowPass here: both mirror, and would free, the sun UBO.
		if (CompositePass *composite = render_.passes().composite) composite->release_targets();
		if (InjectPass *inject = render_.passes().inject) inject->release_targets();
		if (LodRasterPass *lod_raster = render_.passes().lod_raster) lod_raster->release_targets();
		if (GrassRasterPass *grass_raster = render_.passes().grass_raster) grass_raster->release_targets();
	}
	if (!headless_.ensure(rd, in.size) || !headless_.clear(rd)) return out;
	out.scene_color = headless_.color();
	out.scene_depth = headless_.depth();
	return out;
}

bool VoxelFrame::render_headless(RenderingDevice *rd, const FrameInputs &in) {
	const FrameInputs headless = prepare_headless(rd, in);
	if (!headless.scene_color.is_valid()) return false;
	// Both halves run even when the first aborts, exactly as the engine fires both callbacks.
	const bool pre = render_pre_opaque(rd, headless);
	const bool post = render_post_opaque(rd, headless);
	return pre && post;
}

void VoxelFrame::release_gpu() {
	headless_.release();
}
