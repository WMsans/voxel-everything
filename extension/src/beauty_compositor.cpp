#include "beauty_compositor.h"
#include "voxel_world.h"
#include "render/frame.h"
#include "render/orchestrator.h"
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

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
	if (world_path_.is_empty() || !render_data) return;
	VoxelWorld *world = nullptr;
	if (!voxel_try_begin_compositor_callback(world_path_, &world)) return;
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

	normal_roughness_state_ = rsb->has_texture("forward_clustered", "normal_roughness") ? 1 : 0;
	FrameInputs in;
	in.cam = sd->get_cam_transform();
	in.proj = sd->get_cam_projection();
	in.size = rsb->get_internal_size();
	in.scene_color = rsb->get_color_texture();
	in.scene_depth = rsb->get_depth_texture();
	in.normal_roughness = normal_roughness_state_ == 1
			? rsb->get_texture("forward_clustered", "normal_roughness") : RID();
	in.rsb = rsb;
	world->context().render->frame().render_post_opaque(rd, in);
}
