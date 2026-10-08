extends SceneTree
# Reproducible Fjords look check through the SHIPPING render path (spec §9 step 7). Run with:
# godot --path . --resolution 1280x720 -s res://tools/fjord_capture.gd -- --out=/tmp/fjords [--seed=N] [--at=x,z]
# (Not --headless: it needs a real RenderingDevice.)
#
# Writes valley.png, ridge.png and aerial.png -- one pose per reference screenshot -- found
# by raycasting the CPU field on a grid around the origin once the sectors there are baked:
#   valley: on the water at the lowest point, looking along the longest run of low ground;
#   ridge:  8 m over the highest point, looking toward the lowest ground, pitched down;
#   aerial: 250 m over the valley point, looking along the valley, pitched down.
# Prints each pose so a benchmark or a later run can reuse it.
const WATER_Y := 51.2
var _last_settle_frames := 0

func _initialize() -> void:
	call_deferred("capture")

func _arg(name: String, fallback: String) -> String:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--%s=" % name):
			return arg.trim_prefix("--%s=" % name)
	return fallback

func _ground(world: VoxelWorld, x: float, z: float) -> float:
	var hit: Dictionary = world.raycast(Vector3(x, 800.0, z), Vector3.DOWN, 1600.0)
	return (hit["pos"] as Vector3).y if hit["hit"] else INF

func _settle(world: VoxelWorld, frames: int) -> bool:
	var quiet := 0
	_last_settle_frames = 0
	for frame in range(frames):
		await process_frame
		_last_settle_frames += 1
		var s: Dictionary = world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(s.get("requests_pending", 1)) == 0 and int(s.get("builds_in_flight", 1)) == 0 and int(s.get("sector_held", 1)) == 0 else 0
		if quiet >= 30 and int(s.get("draw_pages", 0)) > 0:
			return true
	return false

func _shoot(world: VoxelWorld, camera: Camera3D, player: Node3D, at: Vector3, dir: Vector3, path: String) -> bool:
	player.global_position = at
	camera.global_position = at
	camera.look_at(at + dir)
	if not await _settle(world, 6000):
		push_error("fjord capture did not settle at %s" % at)
		return false
	for frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(path)
	print("FJORD_CAPTURE %s at=%s dir=%s" % [path.get_file(), at, dir])
	return true

func capture() -> void:
	var out := _arg("out", "/tmp/fjords")
	DirAccess.make_dir_recursive_absolute(out)
	var scene: Node = load("res://demo/scenes/main.tscn").instantiate()
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	world.world_type = load("res://demo/world_types/30_fjords.tres")
	world.world_seed = int(_arg("seed", "0"))
	root.add_child(scene)
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	world.set_effect_enabled("ssgi", false)
	world.set_grass_value("wind_strength", 0.0)
	world.set_grass_value("wind_speed", 0.0)
	if not world.hooks().debug_init_physics():
		push_error("fjord capture could not initialize the mesh worker")
		quit(1)
		return
	var camera: Camera3D = player.get_node("Camera3D")
	camera.far = 5000.0
	scene.get_node("HUD").visible = false
	player.global_position = Vector3(0.0, 400.0, 0.0)
	if not await _settle(world, 6000):
		push_error("fjord capture: the world around the origin never settled")
		quit(1)
		return
	print("FJORD_CAPTURE origin settle_frames=%d sector_stats=%s" % [_last_settle_frames, world.hooks().debug_sector_stats()])

	# A 41 x 41 grid at 100 m over +-2 km.
	var low := Vector3(0.0, INF, 0.0)
	var high := Vector3(0.0, -INF, 0.0)
	for i in range(-20, 21):
		for j in range(-20, 21):
			var y := _ground(world, i * 100.0, j * 100.0)
			if y == INF:
				continue
			if y < low.y:
				low = Vector3(i * 100.0, y, j * 100.0)
			if y > high.y:
				high = Vector3(i * 100.0, y, j * 100.0)
	# --at=x,z pins the valley point, so tuning rounds compare the same fjord.
	var at := _arg("at", "").split_floats(",")
	if at.size() == 2:
		low = Vector3(at[0], _ground(world, at[0], at[1]), at[1])
	# The valley's direction: of 16 headings, the one with the longest run of ground under
	# water + 5 m from the low point.
	var best_dir := Vector3(1.0, 0.0, 0.0)
	var best_run := -1
	for k in range(16):
		var a := TAU * k / 16.0
		var d := Vector3(cos(a), 0.0, sin(a))
		var run := 0
		while run < 60 and _ground(world, low.x + d.x * 25.0 * (run + 1), low.z + d.z * 25.0 * (run + 1)) < WATER_Y + 5.0:
			run += 1
		if run > best_run:
			best_run = run
			best_dir = d
	var valley_at := Vector3(low.x, WATER_Y + 3.0, low.z)
	var to_low := Vector3(low.x - high.x, 0.0, low.z - high.z).normalized()
	var ok := await _shoot(world, camera, player, valley_at, best_dir + Vector3(0.0, 0.08, 0.0), out.path_join("valley.png"))
	ok = ok and await _shoot(world, camera, player, high + Vector3(0.0, 8.0, 0.0), to_low + Vector3(0.0, -0.15, 0.0), out.path_join("ridge.png"))
	ok = ok and await _shoot(world, camera, player, valley_at + Vector3(0.0, 250.0, 0.0), best_dir + Vector3(0.0, -0.45, 0.0), out.path_join("aerial.png"))
	print("FJORD_CAPTURE sector stats: ", world.hooks().debug_sector_stats())
	world.shutdown_render_resources()
	await process_frame
	quit(0 if ok else 1)
