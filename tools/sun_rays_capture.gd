extends SceneTree
# Sun rays look and cost check through the SHIPPING render path
# (docs/superpowers/specs/2026-10-23-sun-rays-design.md). Run with:
# godot --path . --resolution 2560x1440 -s res://tools/sun_rays_capture.gd -- --out=/tmp/sun-rays
#
# The shipped sun stands almost straight overhead and white, which never shows rays through a
# tree line. Only here, it is lowered to --elev degrees along the --dir heading and tinted orange
# (user's call: the game's sun is not changed). The camera stands at --at, the far grove (memory:
# trees are rare), and looks straight at the sun, so the crowns stand between them.
#
# Writes on.png / off.png (effect enabled / disabled) and away.png (facing away from the sun,
# effect on), then times --frames frames each with the effect on, off, on (vsync off; GPU
# timestamps are not valid on this machine) and prints SUN_RAYS_TIMING with the p50s.
#
# --shipped-sun keeps the game's own sun (about 85 degrees up, white) instead: the view a player
# gets looking up, where the rays flood the view around the sun.
#
# --knob=name,value sets one SunRays knob (intensity, ray_length, sun_size), repeatable, for A/B.
#
# --block=material puts an r=3 ball of that material (ice, water) 12 m toward the sun, covering
# it: a transparent ball must let the rays through. SUN_RAYS_GAIN is the mean brightness on.png
# adds over off.png in a box around the sun (the screen centre); ~0 means nothing got through.

func _initialize() -> void:
	call_deferred("capture")

func capture() -> void:
	var out := "/tmp/sun-rays-capture"
	var at := Vector3(65.0, 62.0, -930.0)
	var dir := Vector3(-33.0, 0.0, -35.0)
	var elev := 3.0
	var frames := 300
	var knobs := {}
	var shipped_sun := false
	var block := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
		if arg.begins_with("--at="):
			var a := arg.trim_prefix("--at=").split(",")
			at = Vector3(float(a[0]), float(a[1]), float(a[2]))
		if arg.begins_with("--dir="):
			var d := arg.trim_prefix("--dir=").split(",")
			dir = Vector3(float(d[0]), 0.0, float(d[2]))
		if arg.begins_with("--elev="):
			elev = float(arg.trim_prefix("--elev="))
		if arg.begins_with("--knob="):
			var kv := arg.trim_prefix("--knob=").split(",")
			knobs[kv[0]] = float(kv[1])
		if arg.begins_with("--block="):
			block = arg.trim_prefix("--block=")
		if arg == "--shipped-sun":
			shipped_sun = true
		if arg.begins_with("--frames="):
			frames = int(arg.trim_prefix("--frames="))
	DirAccess.make_dir_recursive_absolute(out)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	var scene: Node = load("res://demo/scenes/main.tscn").instantiate()
	root.add_child(scene)
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	if not world.hooks().debug_init_physics():
		push_error("sun rays capture could not initialize the mesh worker")
		quit(1)
		return

	# A DirectionalLight3D shines along its local -Z, so +Z points at the sun.
	var e := deg_to_rad(elev)
	var to_sun := (dir.normalized() * cos(e) + Vector3.UP * sin(e)).normalized()
	var light: DirectionalLight3D = scene.get_node("VoxelWorld/Sun")
	if shipped_sun:
		to_sun = light.global_basis.z.normalized()
	else:
		light.global_basis = Basis.looking_at(-to_sun)
		light.light_color = Color(1.0, 0.55, 0.25)
	# look_at needs an up that is not parallel to the view; the shipped sun is nearly overhead.
	var up := Vector3.UP if absf(to_sun.y) < 0.95 else Vector3.BACK

	var effect: SunRays
	for fx in (scene.get_node("VoxelWorld/WorldEnvironment") as WorldEnvironment).compositor.compositor_effects:
		if fx is SunRays:
			effect = fx
	if effect == null:
		push_error("sun rays capture found no SunRays in the world's compositor")
		quit(1)
		return
	for key in knobs:
		effect.set(key, knobs[key])

	if block != "":
		var id := -1
		for m in world.material_table():
			if m["name"] == block:
				id = m["id"]
		if id < 0:
			push_error("sun rays capture: no material named " + block)
			quit(1)
			return
		world.hooks().debug_apply_sphere_add(at + to_sun * 12.0, 3.0, id)

	var camera: Camera3D = player.get_node("Camera3D")
	player.global_position = at
	camera.global_position = at
	camera.look_at(at + to_sun, up)
	camera.far = 3000.0
	scene.get_node("HUD").visible = false
	var quiet := 0
	var stats: Dictionary
	for frame in range(6000):
		await process_frame
		stats = world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(stats.get("requests_pending", 1)) == 0 and int(stats.get("builds_in_flight", 1)) == 0 else 0
		if quiet >= 30 and int(stats.get("draw_pages", 0)) > 0:
			break
	if quiet < 30:
		push_error("sun rays capture did not settle: %s" % stats)
		quit(1)
		return

	effect.enabled = true
	await _shot(out.path_join("on.png"))
	effect.enabled = false
	await _shot(out.path_join("off.png"))
	print("SUN_RAYS_GAIN %.4f" % _centre_gain(out.path_join("on.png"), out.path_join("off.png")))
	effect.enabled = true
	camera.look_at(at - Vector3(to_sun.x, 0.0, to_sun.z))
	await _shot(out.path_join("away.png"))
	camera.look_at(at + to_sun, up)

	var p50 := []
	for enabled in [true, false, true]:
		effect.enabled = enabled
		p50.append(await _p50_ms(frames))
	print("SUN_RAYS_TIMING p50_ms on=%.2f off=%.2f on=%.2f (%d frames each, elev %.1f, intensity %.2f ray_length %.2f sun_size %.2f)" % [
		p50[0], p50[1], p50[2], frames, elev, effect.intensity, effect.ray_length, effect.sun_size])
	print("SUN_RAYS_CAPTURE saved to ", out, " leaf stats: ", world.hooks().debug_leaf_stats())
	world.shutdown_render_resources()
	await process_frame
	quit()

func _shot(path: String) -> void:
	for frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(path)

func _centre_gain(on_path: String, off_path: String) -> float:
	var on := Image.load_from_file(on_path)
	var off := Image.load_from_file(off_path)
	var half := on.get_height() / 10
	var c := on.get_size() / 2
	var sum := 0.0
	for y in range(c.y - half, c.y + half):
		for x in range(c.x - half, c.x + half):
			sum += on.get_pixel(x, y).get_luminance() - off.get_pixel(x, y).get_luminance()
	return sum / (4.0 * half * half)

func _p50_ms(frames: int) -> float:
	for frame in range(30):
		await process_frame
	var times := PackedFloat64Array()
	var last := Time.get_ticks_usec()
	for frame in range(frames):
		await process_frame
		var now := Time.get_ticks_usec()
		times.append((now - last) / 1000.0)
		last = now
	times.sort()
	return times[times.size() / 2]
