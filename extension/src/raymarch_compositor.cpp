#include "raymarch_compositor.h"
#include "voxel_world.h"
#include "render/frame.h"
#include "render/orchestrator.h"
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/render_scene_data.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

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

	RenderingDevice *rd = RenderingServer::get_singleton()->get_rendering_device();
	RenderSceneBuffersRD *rsb = Object::cast_to<RenderSceneBuffersRD>(render_data->get_render_scene_buffers().ptr());
	RenderSceneData *sd = render_data->get_render_scene_data();
	if (!rd || !rsb || !sd) return;
	// Everything the frame needs from the engine, and nothing else: the stage order and all
	// per-frame packing live in VoxelFrame (render/frame.h).
	FrameInputs in;
	in.cam = sd->get_cam_transform();
	in.proj = sd->get_cam_projection();
	in.size = rsb->get_internal_size();
	in.scene_color = rsb->get_color_texture();
	in.scene_depth = rsb->get_depth_texture();
	in.rsb = rsb;
	world->context().render->frame().render_pre_opaque(rd, in);
}
