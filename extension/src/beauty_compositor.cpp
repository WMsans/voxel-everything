#include "beauty_compositor.h"
#include "voxel_world.h"
#include "render/beauty_camera.h"
#include "render/contact_shadow_pass.h"
#include "render/gbuffer.h"
#include "render/ssr_pass.h"
#include "render/outline_pass.h"
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/variant/projection.hpp>

using namespace godot;

BeautyCompositor::BeautyCompositor() {
	set_effect_callback_type(EFFECT_CALLBACK_TYPE_POST_OPAQUE);
	set_needs_normal_roughness(true);
}

void BeautyCompositor::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_world_path", "p"), &BeautyCompositor::set_world_path);
	ClassDB::bind_method(D_METHOD("get_world_path"), &BeautyCompositor::get_world_path);
	ADD_PROPERTY(PropertyInfo(Variant::NODE_PATH, "world_path"), "set_world_path", "get_world_path");
}

void BeautyCompositor::_render_callback(int cb_type, RenderData *render_data) {
	if (cb_type != EFFECT_CALLBACK_TYPE_POST_OPAQUE) return;
	if (!voxel_compositor_callbacks_enabled()) return;
	if (world_path_.is_empty() || !render_data) return;
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (!tree) return;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(
			tree->get_root()->get_node_or_null(world_path_));
	if (!world || world->get_use_local_device()) return;
	if (!world->try_begin_render_callback()) return;
	struct CallbackGuard {
		VoxelWorld *world;
		~CallbackGuard() { world->end_render_callback(); }
	} callback_guard{world};
	world->ensure_initialized();
	if (!world->is_initialized()) return;
	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	RenderSceneBuffersRD *rsb = Object::cast_to<RenderSceneBuffersRD>(
			render_data->get_render_scene_buffers().ptr());
	RenderSceneData *sd = render_data->get_render_scene_data();
	if (!rd || !rsb || !sd) return;
	const Vector2i size = rsb->get_internal_size();
	if (size.x <= 0 || size.y <= 0) return;
	GpuTimings *timings = world->gpu_timings();
	timings->poll(rd);

	normal_roughness_state_ = rsb->has_texture("forward_clustered", "normal_roughness") ? 1 : 0;
	const RID normal_rough = normal_roughness_state_ == 1
			? rsb->get_texture("forward_clustered", "normal_roughness") : RID();
	// Task 9 found the texture reachable but constant/uncalibrated. Keep dynamic normal
	// creases disabled until a known-orientation calibration promotes the state to 2.
	const bool have_calibrated_normal_roughness = normal_roughness_state_ == 2;
	world->set_normal_roughness_state(normal_roughness_state_);
	world->set_beauty_compositor(this);

	const Transform3D cam = sd->get_cam_transform();
	const Projection proj = sd->get_cam_projection();
	const Projection view(cam.affine_inverse());
	const Projection view_proj = proj * view;
	const float cam_pos[3] = {cam.origin.x, cam.origin.y, cam.origin.z};
	CameraUbo *ubo = world->beauty_camera();
	if (!ubo || !ubo->ensure(rd)) return;
	// Device-level operation: this precedes the contact-shadow compute list.
	ubo->update(rd, view_proj, cam_pos, size, 0.05f, 4000.0f);

	const ve::BeautySettings settings = world->beauty_settings();
	ContactShadowPass *cs = world->contact_shadow_pass();
	if (cs) {
		timings->begin(rd, "contact");
		const bool contact_ok = cs->render(rd, rsb->get_color_texture(), rsb->get_depth_texture(), size,
				ubo->buffer(), settings);
		if (contact_ok) timings->end(rd, "contact");
		else timings->cancel("contact");
	}
	GBuffer *gb = world->gbuffer();
	if (SsrPass *ssr = world->ssr_pass()) {
		timings->begin(rd, "ssr");
		const bool ssr_ok = ssr->render(rd, rsb->get_color_texture(), rsb->get_depth_texture(),
				gb ? gb->surface() : RID(), gb ? gb->depth() : RID(), normal_rough,
				normal_roughness_state_ == 1, ubo->buffer(), size, settings);
		if (ssr_ok) timings->end(rd, "ssr");
		else timings->cancel("ssr");
	}
	if (OutlinePass *outline = world->outline_pass(); outline && gb && gb->is_valid()) {
		timings->begin(rd, "outlines");
		const bool outline_ok = outline->render(rd, rsb->get_color_texture(), rsb->get_depth_texture(),
				gb->depth(), gb->surface(), normal_rough, have_calibrated_normal_roughness,
				ubo->buffer(), size, settings);
		if (outline_ok) timings->end(rd, "outlines");
		else timings->cancel("outlines");
	}
	// Non-visual copy: outline above is the last scene-colour mutation before glow/tonemap.
	if (gb && gb->is_valid()) {
		timings->begin(rd, "history");
		if (world->downsample_history(rd, rsb->get_color_texture(), *gb))
			timings->end(rd, "history");
		else timings->cancel("history");
	}
	timings->end_frame(rd);
}
