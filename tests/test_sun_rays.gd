extends GdUnitTestSuite
# addons/sun_rays: the standalone screen-space sun rays effect
# (docs/superpowers/specs/2026-10-23-sun-rays-design.md).
#
# Every scene here is plain Godot (cubes, a light, a camera) and no voxel class is created: the
# effect has to work in any project, and this suite is the proof. The background is black and the
# boxes are unshaded black, so whatever is not black in a frame is light the effect added.

const ASPECT := 16.0 / 9.0
const FOV := 70.0
const ONE_STEP := 1.0 / 255.0

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

# {viewport, camera, light, effect, environment}. The camera sits at the origin looking down -Z.
func make_scene(size := Vector2i(256, 144)) -> Dictionary:
	var vp := SubViewport.new()
	vp.size = size
	vp.own_world_3d = true
	vp.use_hdr_2d = true
	# A SubViewport inherits the project's render scale (0.5 here); pin 1.0 so a pixel in these
	# tests is a pixel the effect sees.
	vp.scaling_3d_scale = 1.0
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	add_child(vp)
	_nodes.append(vp)
	var env := Environment.new()
	env.background_mode = Environment.BG_COLOR
	env.background_color = Color.BLACK
	env.tonemap_mode = Environment.TONE_MAPPER_LINEAR
	env.glow_enabled = false
	var effect := SunRays.new()
	var compositor := Compositor.new()
	compositor.compositor_effects = [effect]
	var we := WorldEnvironment.new()
	we.environment = env
	we.compositor = compositor
	vp.add_child(we)
	var cam := Camera3D.new()
	cam.fov = FOV
	vp.add_child(cam)
	cam.current = true
	var light := DirectionalLight3D.new()
	vp.add_child(light)
	aim_sun(light, Vector3(0, 0, -1))
	return {"viewport": vp, "camera": cam, "light": light, "effect": effect, "environment": we}

# A DirectionalLight3D shines along its local -Z, so +Z points at the sun.
func aim_sun(light: DirectionalLight3D, to_sun: Vector3) -> void:
	light.basis = Basis.looking_at(-to_sun.normalized(), Vector3.UP if absf(to_sun.normalized().y) < 0.99 else Vector3.BACK)

# An unshaded black box spanning [lo, hi].
func add_box(scene: Dictionary, lo: Vector3, hi: Vector3) -> void:
	var mesh := MeshInstance3D.new()
	var box := BoxMesh.new()
	box.size = hi - lo
	var mat := StandardMaterial3D.new()
	mat.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	mat.albedo_color = Color.BLACK
	box.material = mat
	mesh.mesh = box
	mesh.position = (lo + hi) * 0.5
	(scene["viewport"] as SubViewport).add_child(mesh)

func render(scene: Dictionary) -> Image:
	var vp: SubViewport = scene["viewport"]
	vp.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	return vp.get_texture().get_image()

# [on, off]: the same frame with the effect enabled, then disabled.
func render_on_off(scene: Dictionary) -> Array:
	var on: Image = await render(scene)
	(scene["effect"] as SunRays).enabled = false
	var off: Image = await render(scene)
	(scene["effect"] as SunRays).enabled = true
	return [on, off]

func max_diff(a: Image, b: Image) -> float:
	var worst := 0.0
	for y in range(a.get_height()):
		for x in range(a.get_width()):
			var ca := a.get_pixel(x, y)
			var cb := b.get_pixel(x, y)
			worst = maxf(worst, maxf(absf(ca.r - cb.r), maxf(absf(ca.g - cb.g), absf(ca.b - cb.b))))
	return worst

func luminance(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b

# The sun straight ahead, and a box over the lower-left with its corner just short of the screen
# centre, so open sky surrounds the sun and the box sits right beside it.
func make_sun_ahead_scene(size := Vector2i(256, 144)) -> Dictionary:
	var scene := make_scene(size)
	add_box(scene, Vector3(-20, -20, -10.5), Vector3(-0.3, -0.3, -10))
	return scene

# Mean luminance added over the box's pixels within 0.2 screen heights of the sun (the centre).
func box_gain_near_sun(scene: Dictionary, on: Image, off: Image) -> float:
	var cam: Camera3D = scene["camera"]
	var vp: SubViewport = scene["viewport"]
	var corner := cam.unproject_position(Vector3(-0.3, -0.3, -10))
	var centre := Vector2(vp.size) * 0.5
	var sum := 0.0
	var n := 0
	for y in range(on.get_height()):
		for x in range(on.get_width()):
			if x > corner.x - 1 or y < corner.y + 1:
				continue
			if Vector2(x, y).distance_to(centre) > 0.2 * vp.size.y:
				continue
			sum += luminance(on.get_pixel(x, y)) - luminance(off.get_pixel(x, y))
			n += 1
	assert_int(n).override_failure_message("no box pixels near the sun").is_greater(0)
	return sum / maxf(n, 1)

func test_sun_uv_projects_the_light_direction() -> void:
	var proj := Projection.create_perspective(FOV, ASPECT, 0.05, 100.0)
	var ahead := SunRays.sun_uv(Transform3D.IDENTITY, proj, Vector3(0, 0, -1))
	assert_vector(ahead).is_equal_approx(Vector2(0.5, 0.5), Vector2(0.001, 0.001))
	var up := SunRays.sun_uv(Transform3D.IDENTITY, proj, Vector3(0, sin(deg_to_rad(20.0)), -cos(deg_to_rad(20.0))))
	assert_float(up.x).is_equal_approx(0.5, 0.001)
	assert_float(up.y).is_less(0.5)
	var behind := SunRays.sun_uv(Transform3D.IDENTITY, proj, Vector3(0, 0, 1))
	assert_bool(is_inf(behind.x)).is_true()

func test_edge_fade_is_continuous_and_reaches_zero() -> void:
	assert_float(SunRays.edge_fade(Vector2(0.5, 0.5), ASPECT, 0.6)).is_equal(1.0)
	assert_float(SunRays.edge_fade(Vector2(1.0, 0.5), ASPECT, 0.6)).is_equal(1.0)
	assert_float(SunRays.edge_fade(Vector2(1.0 + 0.6 / ASPECT + 0.01, 0.5), ASPECT, 0.6)).is_equal(0.0)
	assert_float(SunRays.edge_fade(Vector2(INF, INF), ASPECT, 0.6)).is_equal(0.0)
	var prev := SunRays.edge_fade(Vector2(0.9, 0.5), ASPECT, 0.6)
	var x := 0.905
	while x <= 1.5:
		var f := SunRays.edge_fade(Vector2(x, 0.5), ASPECT, 0.6)
		assert_float(prev - f).is_greater_equal(0.0)
		assert_float(prev - f).is_less(0.05)
		prev = f
		x += 0.005

func test_sun_ahead_adds_rays_beside_an_occluder() -> void:
	var scene := make_sun_ahead_scene()
	var frames: Array = await render_on_off(scene)
	assert_float(box_gain_near_sun(scene, frames[0], frames[1])).is_greater(0.05)

func test_rays_take_the_light_colour() -> void:
	var scene := make_sun_ahead_scene()
	(scene["light"] as DirectionalLight3D).light_color = Color(1.0, 0.5, 0.1)
	var frames: Array = await render_on_off(scene)
	var corner := (scene["camera"] as Camera3D).unproject_position(Vector3(-0.3, -0.3, -10))
	var p := Vector2i(int(corner.x) - 2, int(corner.y) + 2)
	var on: Color = frames[0].get_pixel(p.x, p.y)
	var off: Color = frames[1].get_pixel(p.x, p.y)
	var added := Vector3(on.r - off.r, on.g - off.g, on.b - off.b)
	assert_float(added.x).is_greater(added.y)
	assert_float(added.y).is_greater(added.z)
	assert_float(added.z).is_greater(-0.001)

func test_disabled_changes_nothing() -> void:
	var scene := make_sun_ahead_scene()
	(scene["effect"] as SunRays).enabled = false
	var disabled: Image = await render(scene)
	((scene["environment"] as WorldEnvironment).compositor as Compositor).compositor_effects = []
	var absent: Image = await render(scene)
	assert_float(max_diff(disabled, absent)).is_less(ONE_STEP)

func test_sun_behind_camera_changes_nothing() -> void:
	var scene := make_sun_ahead_scene()
	aim_sun(scene["light"], Vector3(0, 0, 1))
	var frames: Array = await render_on_off(scene)
	assert_float(max_diff(frames[0], frames[1])).is_less(ONE_STEP)

func test_fully_occluded_view_changes_nothing() -> void:
	var scene := make_scene()
	add_box(scene, Vector3(-50, -50, -10.5), Vector3(50, 50, -10))
	var frames: Array = await render_on_off(scene)
	assert_float(max_diff(frames[0], frames[1])).is_less(ONE_STEP)

func test_no_directional_light_changes_nothing() -> void:
	var scene := make_sun_ahead_scene()
	(scene["light"] as Node).free()
	var frames: Array = await render_on_off(scene)
	assert_float(max_diff(frames[0], frames[1])).is_less(ONE_STEP)

func test_far_from_the_sun_changes_nothing() -> void:
	var scene := make_scene()
	# 41 degrees left of the view axis puts the sun near uv.x = 0.15 at fov 70, 16:9.
	var a := deg_to_rad(41.0)
	aim_sun(scene["light"], Vector3(-sin(a), 0, -cos(a)))
	add_box(scene, Vector3(0, -50, -10.5), Vector3(50, 50, -10))
	var frames: Array = await render_on_off(scene)
	var effect: SunRays = scene["effect"]
	var sun := SunRays.sun_uv(Transform3D.IDENTITY, (scene["camera"] as Camera3D).get_camera_projection(),
		(scene["light"] as DirectionalLight3D).basis.z)
	assert_float(sun.x).is_between(0.1, 0.2)
	var on: Image = frames[0]
	var off: Image = frames[1]
	var worst := 0.0
	for y in range(on.get_height()):
		for x in range(on.get_width()):
			var uv := Vector2((x + 0.5) / on.get_width(), (y + 0.5) / on.get_height())
			if ((uv - sun) * Vector2(ASPECT, 1.0)).length() <= effect.ray_length + 0.05:
				continue
			worst = maxf(worst, absf(luminance(on.get_pixel(x, y)) - luminance(off.get_pixel(x, y))))
	assert_float(worst).is_less(ONE_STEP)

func test_hidden_light_stops_the_rays() -> void:
	var scene := make_sun_ahead_scene()
	var lit: Array = await render_on_off(scene)
	assert_float(box_gain_near_sun(scene, lit[0], lit[1])).is_greater(0.05)
	(scene["light"] as DirectionalLight3D).visible = false
	var frames: Array = await render_on_off(scene)
	assert_float(max_diff(frames[0], frames[1])).is_less(ONE_STEP)

func test_one_pixel_gap_throws_no_rays() -> void:
	var scene := make_scene()
	# One internal pixel at 10 m, for the vertical fov over a 144-pixel-high view. The slit covers
	# the column just right of centre, so no pixel centre sits on its edge.
	var px := 2.0 * 10.0 * tan(deg_to_rad(FOV * 0.5)) / 144.0
	add_box(scene, Vector3(-50, -50, -10.5), Vector3(0.0, 50, -10))
	add_box(scene, Vector3(px, -50, -10.5), Vector3(50, 50, -10))
	var frames: Array = await render_on_off(scene)
	assert_float(max_diff(frames[0], frames[1])).is_less(ONE_STEP)

func test_rays_line_up_at_half_render_scale() -> void:
	var scene := make_sun_ahead_scene()
	(scene["viewport"] as SubViewport).scaling_3d_scale = 0.5
	var frames: Array = await render_on_off(scene)
	assert_float(box_gain_near_sun(scene, frames[0], frames[1])).is_greater(0.05)

func test_resize_keeps_rays_working() -> void:
	var scene := make_sun_ahead_scene()
	await render(scene)
	(scene["viewport"] as SubViewport).size = Vector2i(320, 180)
	var frames: Array = await render_on_off(scene)
	assert_float(box_gain_near_sun(scene, frames[0], frames[1])).is_greater(0.05)

# Every other frame here has the sun on the horizontal midline, where a flipped screen y would
# go unnoticed. Raise it and the glow must sit where the sun is, not in its mirror image.
func test_rays_come_from_where_the_sun_is_on_screen() -> void:
	var scene := make_scene()
	var a := deg_to_rad(15.0)
	aim_sun(scene["light"], Vector3(0, sin(a), -cos(a)))
	# A horizon just under the sun, so something partly blocks it.
	add_box(scene, Vector3(-50, -50, -10.5), Vector3(50, 10.0 * tan(a) - 0.3, -10))
	var frames: Array = await render_on_off(scene)
	var on: Image = frames[0]
	var off: Image = frames[1]
	var sun := (scene["camera"] as Camera3D).unproject_position(Vector3(0, sin(a), -cos(a)) * 10.0)
	assert_float(sun.y).is_less(on.get_height() * 0.4)
	var x := on.get_width() / 2
	var at_sun := luminance(on.get_pixel(x, int(sun.y))) - luminance(off.get_pixel(x, int(sun.y)))
	var mirror := on.get_height() - 1 - int(sun.y)
	var at_mirror := luminance(on.get_pixel(x, mirror)) - luminance(off.get_pixel(x, mirror))
	assert_float(at_sun).is_greater(at_mirror + 0.05)

# Spec anti-goal: looking up at the sun through open sky must not wash the screen out, and the
# effect draws no sun disc (user's call). Nothing blocks the sun, so there is no ray to draw.
func test_open_sky_at_the_sun_is_not_washed_out() -> void:
	var scene := make_scene()
	var env: Environment = (scene["environment"] as WorldEnvironment).environment
	env.background_color = Color(0.85, 0.85, 0.85)
	var frames: Array = await render_on_off(scene)
	var on: Image = frames[0]
	var brightest := 0.0
	for y in range(on.get_height()):
		for x in range(on.get_width()):
			var c := on.get_pixel(x, y)
			brightest = maxf(brightest, maxf(c.r, maxf(c.g, c.b)))
	assert_float(brightest).is_less(1.1)
	assert_float(max_diff(on, frames[1])).is_less(0.02)
