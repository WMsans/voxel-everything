extends SceneTree
#   godot --path . --resolution 1280x720 -s res://tools/render_scale_repro.gd -- --out=/tmp/rs-repro

var _out := "/tmp/rs-repro"
var _world: VoxelWorld

func _initialize() -> void:
	call_deferred("capture")

func settle(world: VoxelWorld) -> bool:
	var quiet := 0
	for frame in range(6000):
		await process_frame
		var stats: Dictionary = world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(stats.get("requests_pending", 1)) == 0 and int(stats.get("builds_in_flight", 1)) == 0 else 0
		if quiet >= 30 and int(stats.get("draw_pages", 0)) > 0:
			return true
	return false

func measure(img: Image) -> Array:
	var w := img.get_width()
	var h := img.get_height()
	var nb := 0
	var nw := 0
	var n := 0
	for y in range(0, h, 3):
		for x in range(0, w, 3):
			var c := img.get_pixel(x, y)
			n += 1
			if c.r < 0.03 and c.g < 0.03 and c.b < 0.03: nb += 1
			elif c.r > 0.98 and c.g > 0.98 and c.b > 0.98: nw += 1
	return [nb * 100.0 / n, nw * 100.0 / n]

func check(label: String) -> void:
	await RenderingServer.frame_post_draw
	var img := root.get_texture().get_image()
	var m := measure(img)
	var flag := "  <<< ARTIFACT" if (m[0] > 2.0 and m[1] > 2.0) else ""
	print("REPRO %-24s %s black=%5.2f%% white=%5.2f%%%s" % [label, img.get_size(), m[0], m[1], flag])
	img.save_png(_out.path_join(label.replace(" ", "_").replace("=", "").replace(".", "") + ".png"))

func wait(n: int) -> void:
	for frame in range(n): await process_frame

func capture() -> void:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			_out = arg.trim_prefix("--out=")
	DirAccess.make_dir_recursive_absolute(_out)
	var scene: Node = load("res://demo/main.tscn").instantiate()
	root.add_child(scene)
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	_world = world
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	if not world.hooks().debug_init_physics():
		quit(1)
		return
	var camera: Camera3D = player.get_node("Camera3D")
	# Stand ON the ground, like the player does, so the near field fills the frame.
	player.global_position = Vector3(500, 400, 500)
	camera.global_position = player.global_position
	camera.far = 6000.0
	scene.get_node("HUD").visible = false
	await wait(30)
	var hit: Dictionary = world.hooks().debug_raycast(Vector3(500, 400, 500), Vector3(0, -1, 0))
	var ground := 122.0
	if hit.get("hit", false):
		ground = (hit["pos"] as Vector3).y
	print("REPRO ground at ", ground)
	player.global_position = Vector3(500, ground + 1.7, 500)
	camera.global_position = player.global_position
	# The reported view: looking down at close ground, near field dominant.
	camera.look_at(camera.global_position + Vector3(0.4, -0.55, -1.0))
	# A ground-level camera keeps the near field streaming, so wait a fixed budget rather
	# than for quiet.
	await wait(240)
	var tool_node: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	world.add_child(tool_node)
	var aim: Dictionary = world.hooks().debug_raycast(
		camera.global_position, -camera.global_transform.basis.z)
	if aim.get("hit", false):
		var p: Vector3 = aim["pos"]
		for i in range(20):
			tool_node.apply_sphere_subtract(p + Vector3(randf() * 6 - 3, 0, randf() * 6 - 3), 3.0)
		print("REPRO carved at ", p)
	await wait(60)
	await check("base")
	var modes := {
		"bilinear": Viewport.SCALING_3D_MODE_BILINEAR,
		"fsr1": Viewport.SCALING_3D_MODE_FSR,
		"fsr2": Viewport.SCALING_3D_MODE_FSR2,
		"mfx_spatial": Viewport.SCALING_3D_MODE_METALFX_SPATIAL,
		"mfx_temporal": Viewport.SCALING_3D_MODE_METALFX_TEMPORAL,
	}
	for name in modes:
		root.scaling_3d_mode = modes[name]
		root.scaling_3d_scale = 0.65
		await wait(30); await check("%s 065" % name)
		# ...and the reported trigger: change the render scale while this upscaler is live.
		root.scaling_3d_scale = 0.58
		await wait(2);  await check("%s 058 f2" % name)
		await wait(30); await check("%s 058 f32" % name)
	world.shutdown_render_resources()
	await process_frame
	quit()
