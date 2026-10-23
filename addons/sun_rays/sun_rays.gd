@tool
class_name SunRays
extends CompositorEffect
## Screen-space sun rays ("god rays"): light streaming from the sun past whatever stands in front
## of it, as in GPU Gems 3 ch. 13 / the Kodeco "volumetric light scattering" article.
##
## Self-contained: copy this file into any Forward+ project and add a SunRays to a
## WorldEnvironment's Compositor. The sun is the first visible DirectionalLight3D in the scene,
## and its colour and energy tint the rays. "Can this pixel see the sky" is read from the depth
## buffer alone (reverse-Z far plane, depth 0), so anything that writes depth occludes.
##
## A renderer that writes see-through surfaces (water, glass) into depth can say so: create an
## R32F texture "clear_depth" in the scene buffers' "sun_rays" context, holding each such
## surface's depth where only sky is behind it and 0 elsewhere. A pixel whose depth matches it
## counts as sky; anything drawn in front of the surface still blocks.
##
## Two compute dispatches: a low-resolution march toward the sun that counts the sky it passes
## near the sun, then a full-resolution add into the HDR colour before glow and tonemapping.

## Low-resolution march divisor, and steps per march.
const DIVISOR := 2
const STEPS := 64

@export_range(0.0, 4.0, 0.01) var intensity := 1.5
## Screen heights from the sun beyond which nothing is added.
@export_range(0.1, 2.0, 0.01) var ray_length := 0.9
## Screen heights around the sun that emit. Sky farther from the sun throws no rays.
@export_range(0.02, 0.5, 0.01) var sun_size := 0.08

# Written on the main thread before each frame draws, read by the render callback.
var _light: DirectionalLight3D
var _has_sun := false
var _to_sun := Vector3.ZERO
var _sun_color := Color.BLACK

var _rd: RenderingDevice
var _march_shader: RID
var _march_pipeline: RID
var _composite_shader: RID
var _composite_pipeline: RID
var _linear: RID
var _nearest: RID
var _failed := false

func _init() -> void:
	effect_callback_type = EFFECT_CALLBACK_TYPE_POST_TRANSPARENT
	RenderingServer.frame_pre_draw.connect(_on_frame_pre_draw)

func _notification(what: int) -> void:
	if what != NOTIFICATION_PREDELETE:
		return
	# The frame_pre_draw connection goes with the object. Pipelines and cached uniform sets
	# depend on these and go with them.
	for rid in [_march_shader, _composite_shader, _linear, _nearest]:
		if rid.is_valid():
			_rd.free_rid(rid)

## Screen UV of the sun ((0,0) top-left, y down) seen through a camera at `cam` with `proj`, or
## Vector2(INF, INF) when the sun is behind the camera. `to_sun` points from the scene at the sun.
static func sun_uv(cam: Transform3D, proj: Projection, to_sun: Vector3) -> Vector2:
	var v := cam.basis.inverse() * to_sun
	var clip := proj * Vector4(v.x, v.y, v.z, 0.0)
	if clip.w <= 0.0:
		return Vector2(INF, INF)
	return Vector2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5)

## 1 while the sun is on screen, falling smoothly to 0 once it is `reach` screen heights past the
## edge, where no ray could reach the view any more.
static func edge_fade(uv: Vector2, aspect: float, reach: float) -> float:
	if not (is_finite(uv.x) and is_finite(uv.y)):
		return 0.0
	var outside := Vector2(maxf(maxf(-uv.x, uv.x - 1.0), 0.0) * aspect, maxf(maxf(-uv.y, uv.y - 1.0), 0.0))
	return 1.0 - smoothstep(0.0, reach, outside.length())

func _on_frame_pre_draw() -> void:
	if not (is_instance_valid(_light) and _light.is_inside_tree() and _light.is_visible_in_tree()):
		_light = _find_light()
	_has_sun = _light != null
	if _has_sun:
		_to_sun = _light.global_basis.z
		_sun_color = _light.light_color * _light.light_energy

# ponytail: the first visible light wins, and with none in the scene this searches every frame;
# add a group export for several lights, and throttle if a light-less scene ever profiles it.
static func _find_light() -> DirectionalLight3D:
	var tree := Engine.get_main_loop() as SceneTree
	if tree == null:
		return null
	var top: Node = tree.edited_scene_root if Engine.is_editor_hint() else tree.root
	if top == null:
		return null
	for node in top.find_children("*", "DirectionalLight3D", true, false):
		if (node as DirectionalLight3D).is_visible_in_tree():
			return node
	return null

func _render_callback(callback_type: int, render_data: RenderData) -> void:
	if callback_type != EFFECT_CALLBACK_TYPE_POST_TRANSPARENT or not _has_sun:
		return
	var rsb := render_data.get_render_scene_buffers() as RenderSceneBuffersRD
	var scene := render_data.get_render_scene_data()
	if rsb == null or scene == null:
		return
	var size := rsb.get_internal_size()
	if size.x <= 0 or size.y <= 0:
		return
	var aspect := float(size.x) / size.y
	var sun := sun_uv(scene.get_cam_transform(), scene.get_cam_projection(), _to_sun)
	# The scene buffers hold the frame upside down relative to the screen (depth and colour
	# alike), so the march works in their rows, not the screen's.
	sun.y = 1.0 - sun.y
	var fade := edge_fade(sun, aspect, ray_length)
	if fade <= 0.0 or intensity <= 0.0 or not _ensure_pipelines():
		return

	var rays_size := Vector2i(ceili(size.x / float(DIVISOR)), ceili(size.y / float(DIVISOR)))
	if not rsb.has_texture(&"sun_rays", &"rays"):
		rsb.create_texture(&"sun_rays", &"rays", RenderingDevice.DATA_FORMAT_R16_SFLOAT,
			RenderingDevice.TEXTURE_USAGE_SAMPLING_BIT | RenderingDevice.TEXTURE_USAGE_STORAGE_BIT,
			RenderingDevice.TEXTURE_SAMPLES_1, rays_size, 1, 1, true, false)
	var rays := rsb.get_texture(&"sun_rays", &"rays")

	var depth := rsb.get_depth_layer(0)
	var has_clear := rsb.has_texture(&"sun_rays", &"clear_depth")
	var clear := rsb.get_texture(&"sun_rays", &"clear_depth") if has_clear else depth
	var march_set := UniformSetCacheRD.get_cache(_march_shader, 0, [
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 0, [_nearest, depth]),
		_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 1, [rays]),
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 2, [_nearest, clear]),
	])
	var march_params := PackedFloat32Array([sun.x, sun.y, rays_size.x, rays_size.y,
		aspect, sun_size, ray_length, 1.0 if has_clear else 0.0]).to_byte_array()
	_dispatch(_march_pipeline, march_set, march_params, rays_size)

	var tint := _sun_color * intensity * fade
	var composite_set := UniformSetCacheRD.get_cache(_composite_shader, 0, [
		_uniform(RenderingDevice.UNIFORM_TYPE_IMAGE, 0, [rsb.get_color_layer(0)]),
		_uniform(RenderingDevice.UNIFORM_TYPE_SAMPLER_WITH_TEXTURE, 1, [_linear, rays]),
	])
	var composite_params := PackedFloat32Array([tint.r, tint.g, tint.b, 0.0,
		size.x, size.y, 0.0, 0.0]).to_byte_array()
	_dispatch(_composite_pipeline, composite_set, composite_params, size)

func _dispatch(pipeline: RID, uniform_set: RID, params: PackedByteArray, size: Vector2i) -> void:
	var list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(list, pipeline)
	_rd.compute_list_bind_uniform_set(list, uniform_set, 0)
	_rd.compute_list_set_push_constant(list, params, params.size())
	_rd.compute_list_dispatch(list, ceili(size.x / 8.0), ceili(size.y / 8.0), 1)
	_rd.compute_list_end()

static func _uniform(type: RenderingDevice.UniformType, binding: int, ids: Array) -> RDUniform:
	var u := RDUniform.new()
	u.uniform_type = type
	u.binding = binding
	for id in ids:
		u.add_id(id)
	return u

func _ensure_pipelines() -> bool:
	if _composite_pipeline.is_valid():
		return true
	if _failed:
		return false
	_rd = RenderingServer.get_rendering_device()
	if _rd == null:
		_failed = true
		return false
	_march_shader = _compile(MARCH_GLSL % STEPS)
	_composite_shader = _compile(COMPOSITE_GLSL)
	if not (_march_shader.is_valid() and _composite_shader.is_valid()):
		_failed = true
		return false
	_march_pipeline = _rd.compute_pipeline_create(_march_shader)
	_composite_pipeline = _rd.compute_pipeline_create(_composite_shader)
	var sampler := RDSamplerState.new()
	sampler.repeat_u = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	sampler.repeat_v = RenderingDevice.SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE
	_nearest = _rd.sampler_create(sampler)
	sampler.min_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	sampler.mag_filter = RenderingDevice.SAMPLER_FILTER_LINEAR
	_linear = _rd.sampler_create(sampler)
	return true

func _compile(code: String) -> RID:
	var source := RDShaderSource.new()
	source.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	source.source_compute = code
	var spirv := _rd.shader_compile_spirv_from_source(source)
	if spirv.compile_error_compute != "":
		push_error("SunRays: shader failed to compile: " + spirv.compile_error_compute)
		return RID()
	return _rd.shader_create_from_spirv(spirv)

# One texel of the low-resolution rays texture: march from it to the sun and count the sky passed
# within sun_size of the sun. A step counts as sky only when all four depth texels gathered there
# are sky, so a hairline hole in solid ground (one pixel of depth 0) never emits. Sky is depth 0,
# or the see-through depth when the renderer publishes one (see the class doc).
const MARCH_GLSL := """
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D depth_tex;
layout(r16f, set = 0, binding = 1) uniform restrict writeonly image2D rays_img;
layout(set = 0, binding = 2) uniform sampler2D clear_tex;
layout(push_constant, std430) uniform Params {
	vec2 sun_uv;
	vec2 rays_size;
	float aspect;
	float sun_size;
	float ray_length;
	float has_clear;
} p;

#define STEPS %d

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, ivec2(p.rays_size)))) {
		return;
	}
	vec2 uv = (vec2(px) + 0.5) / p.rays_size;
	vec2 heights = vec2(p.aspect, 1.0); // uv offsets to screen heights
	float dist = length((uv - p.sun_uv) * heights);
	float reach = max(0.0, 1.0 - dist / p.ray_length);
	// How much of the sun's neighbourhood this pixel sees along its line to the sun: 1 through
	// open sky, 0 behind a trunk. That fraction, not the raw count, is what keeps a streak
	// bright along its whole length; the reach falloff alone ends it. Only the stretch within
	// sun_size of the sun can emit, so every step lands there: spread along the whole line, a
	// far pixel would put a handful of steps on it and quantize into blocks.
	float lit = 0.0;
	float total = 0.0;
	if (reach > 0.0) {
		vec2 along = dist > 0.0 ? (uv - p.sun_uv) / dist : vec2(0.0); // uv per screen height, sun to pixel
		float span = min(dist, p.sun_size);
		for (int i = 0; i < STEPS; i++) {
			float t = span * (float(i) + 0.5) / float(STEPS);
			vec2 s = p.sun_uv + along * t;
			if (any(lessThan(s, vec2(0.0))) || any(greaterThan(s, vec2(1.0)))) {
				continue; // off screen: unknown, so neither sky nor blocked
			}
			float w = 1.0 - t / p.sun_size;
			w *= w;
			total += w;
			vec4 d = textureGather(depth_tex, s, 0);
			if (p.has_clear > 0.5 && any(notEqual(d, vec4(0.0)))) {
				d = min(d, abs(d - textureGather(clear_tex, s, 0))); // plain sky skips the fetch
			}
			if (all(equal(d, vec4(0.0)))) {
				lit += w;
			}
		}
	}
	imageStore(rays_img, px, vec4(total > 0.0 ? lit / total * reach * reach : 0.0));
}
"""

const COMPOSITE_GLSL := """
#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(rgba16f, set = 0, binding = 0) uniform restrict image2D color_img;
layout(set = 0, binding = 1) uniform sampler2D rays_tex;
layout(push_constant, std430) uniform Params {
	vec4 tint;
	vec2 size;
	vec2 pad;
} p;

void main() {
	ivec2 px = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(px, ivec2(p.size)))) {
		return;
	}
	vec4 c = imageLoad(color_img, px);
	c.rgb += texture(rays_tex, (vec2(px) + 0.5) / p.size).r * p.tint.rgb;
	imageStore(color_img, px, c);
}
"""
