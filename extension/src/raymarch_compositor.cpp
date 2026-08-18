#include "raymarch_compositor.h"
#include "voxel_world.h"
#include "render/composite_pass.h"
#include "render/gpu_atlas.h"
#include "render/hiz_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/lod_cull_pass.h"
#include "lod/lod_tree.h"
#include "render/island_cull_pass.h"
#include "render/raymarch_pass.h"
#include "render/world_streamer.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <cmath>

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
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) return;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(tree->get_root()->get_node_or_null(world_path_));
	if (!world || world->get_use_local_device()) return;

	// Runs on the render thread (PRE_OPAQUE fires between the depth pre-pass and the opaque
	// pass, outside any engine draw list); the main RenderingDevice is safe to use here.
	// ensure_initialized() is a no-op after the first frame.
	world->ensure_initialized();
	if (!world->is_initialized()) return;

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	RenderSceneBuffersRD *rsb = Object::cast_to<RenderSceneBuffersRD>(render_data->get_render_scene_buffers().ptr());
	RenderSceneData *sd = render_data->get_render_scene_data();
	if (!rd || !rsb || !sd) return;
	const Vector2i size = rsb->get_internal_size();
	if (size.x <= 0 || size.y <= 0) return;

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
	if (!std::isfinite(tan_x) || !std::isfinite(tan_y) || tan_x <= 0.0f || tan_y <= 0.0f) return; // ortho/degenerate

	ve::CameraParams cp{};
	const Vector3 right = cam.basis.get_column(0);
	const Vector3 up = cam.basis.get_column(1);
	const Vector3 fwd = -cam.basis.get_column(2);
	cp.cam_pos[0] = cam.origin.x; cp.cam_pos[1] = cam.origin.y; cp.cam_pos[2] = cam.origin.z;
	cp.cam_right[0] = right.x; cp.cam_right[1] = right.y; cp.cam_right[2] = right.z;
	cp.cam_up[0] = up.x; cp.cam_up[1] = up.y; cp.cam_up[2] = up.z;
	cp.cam_fwd[0] = fwd.x; cp.cam_fwd[1] = fwd.y; cp.cam_fwd[2] = fwd.z;
	cp.params[0] = tan_x; cp.params[1] = tan_y; cp.params[2] = 200.0f;
	const Vector3i sr = world->get_world_size_regions();
	cp.dims[0] = sr.x; cp.dims[1] = sr.y; cp.dims[2] = sr.z;
	cp.dims[3] = world->island_slot_count();
	const Vector3i ob = world->get_world_origin_bricks();
	cp.region_origin[0] = ob.x / 32; cp.region_origin[1] = ob.y / 32;
	cp.region_origin[2] = ob.z / 32;
	cp.region_origin[3] = 0; // Task 11 sets the cull grid
	const Vector3i ab = world->get_atlas_bricks();
	cp.atlas_bricks[0] = ab.x; cp.atlas_bricks[1] = ab.y; cp.atlas_bricks[2] = ab.z;

	// Volumes before anything that evaluates the field: an op naming a slot may already be
	// in the edit log, and the streamer is about to regenerate the bricks that read it.
	world->drain_island_uploads(rd);
	WorldStreamer *st = world->streamer();
	if (st) st->run_frame(rd, cam.origin.x, cam.origin.y, cam.origin.z);

	RaymarchPass *rmp = world->raymarch_pass();
	GpuAtlas *atlas = world->atlas();
	CompositePass *cmp = world->composite_pass();
	MaterialAtlas *materials = world->material_atlas();
	if (!rmp || !atlas || !cmp || !materials) return;

	float edit_state[6] = {0, 0, 0, 0, 0, 0};
	if (st && st->last_edit_radius() > 0.0f) {
		edit_state[0] = st->last_edit_center()[0];
		edit_state[1] = st->last_edit_center()[1];
		edit_state[2] = st->last_edit_center()[2];
		edit_state[3] = st->last_edit_radius();
		edit_state[4] = static_cast<float>(st->last_edit_type());
		edit_state[5] = static_cast<float>(st->last_edit_material());
	}

	const int rw = static_cast<int>(size.x * 0.66f);
	const int rh = static_cast<int>(size.y * 0.66f);
	if (rw <= 0 || rh <= 0) return;
	const int islands = world->island_slot_count();
	IslandCullPass *cull = world->island_cull();
	RID mask;
	if (cull && islands > 0 && cull->render(rd, *world->islands(), cp, rw, rh, islands)) {
		mask = cull->mask_buffer();
		cp.region_origin[3] = cull->tiles_x();
		cp.atlas_bricks[3] = cull->tiles_y();
	}
	cp.dims[3] = islands;
	if (!rmp->render(rd, *atlas, world->islands(), mask, cp, rw, rh, edit_state)) return;

	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	// Master-API note: rsb->get_color_texture()/get_depth_texture() exist on godot-cpp master
	// (render_scene_buffers_rd.hpp) and return the non-MSAA internal color/depth textures —
	// the same RIDs the engine's own framebuffers use when MSAA is disabled (verified against
	// render_forward_clustered.cpp), so the composite writes into the actual scene buffers.
	const float cam_pos[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
	cmp->draw(rd, rsb->get_color_texture(), rsb->get_depth_texture(),
			rmp->color_texture(), rmp->hitpos_texture(), view_proj, *materials,
			cam_pos, ve::kLodFadeStartM, ve::kLodFadeEndM);

	// Far field: after the composite the scene depth holds exact near-field occluders. Build
	// the HiZ pyramid from it for the GPU cull (Task 15) and the coarse async readback for the
	// CPU walk, then draw the LoD raster against the same depth buffer. The CPU walk gets the
	// occlusion interface: stale readback may delay a build, never hide a chunk.
	HizPass *hiz = world->hiz_pass();
	if (hiz) hiz->build(rd, rsb->get_depth_texture(), size);
	LodRasterPass *lod_raster = world->lod_raster_pass();
	LodCullPass *lod_cull = world->lod_cull_pass();
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
		// Ordering: the indirect args upload (a device-level command) must precede the
		// cull's compute list, and the cull's compute list must end before the raster's
		// draw list opens. draw() only issues the indirect draw.
		world->lod_pool()->upload_draw_args(lod_raster->draw_pages());
		if (lod_cull) lod_cull->run(rd, *world->lod_pool(), hiz, view_proj,
				lod_raster->draw_page_count());
		lod_raster->draw(rd, *world->lod_pool(), *materials,
				rsb->get_color_texture(), rsb->get_depth_texture(), view_proj, cam_pos,
				lod_raster->draw_page_count(), ve::kLodFadeStartM, ve::kLodFadeEndM);
	}
}
