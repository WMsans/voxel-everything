extends SceneTree
# Reproducible, GPU-backed look check for transparent materials, through the SHIPPING scene
# (demo/main.tscn) rather than the debug hooks. Run with:
#   godot --path . --resolution 1600x900 -s res://tools/transparency_capture.gd -- --out=reports/transparency-B --case=seam
# and the same line with --case=foliage / inside / sky / linger.
#
# --case=seam     three r=7 ice probe balls strung out along one sight line -- one inside
#                 the near field, one in the fade band, one past it -- PLUS a chain of
#                 overlapping r=5 balls every 6 m from 20 m to 146 m (range(20, 150, 6)),
#                 so a single frame holds one continuous ice ridge crossing the band
#                 (near-shell ice in front of it, far-shell ice behind) with the dithered
#                 cross-fade between them.
#                 What to look for: a line of double-dark or missing pixels along the
#                 band, or ice that changes brightness abruptly at it.
# --case=foliage  an ice patch ahead of a low camera with the shipped grass scatter on, so
#                 grass blades stand between the lens and the ice. Spec §7 step 1: a raster
#                 front nearer than the G-buffer depth leaves the pixel alone, so the blades
#                 must draw over the ice untinted.
# --case=inside   the lens INSIDE an r=6 ball: every pixel has thickness and no front, so the
#                 composite must tint the world and stay finite -- the black-screen case.
# --case=sky      one r=7 ball floating 45 m up at ~110 m, past the shell's fade band and
#                 with nothing opaque behind it: the far shell owns it, and the sky has to
#                 come through tinted rather than as a hole.
# --case=linger   TWO frames of one frame's worth of ice. `linger_before` is settled with a
#                 chain of ice balls in front of the lens; then every one of them is
#                 subtracted and the world is settled again for `linger_after`. The G-buffer
#                 resolve writes depth and nothing ever clears it, so if any ice outline,
#                 tint or reflection survives in `linger_after`, that is a stale pixel the
#                 frame after next will still be drawing. A fixed camera is the exposure: the
#                 shipped test cameras are fixed too, which is why this is the one case here
#                 that can be wrong in a still.
#
# The seam probe in tests/test_lod_seam.gd marks terrain ownership, not front ownership, so
# it cannot answer either question. This writes a frame a person (or an agent) can look at.
# It asserts nothing: the PNG is the evidence, and what it proves is that the shipping path
# produced it.

const EYE := 1.5

# Settled stats and the world, kept on the node because a helper cannot rebind the caller's
# local. _linger_spheres is what `linger` takes back out for its second frame.
var _stats: Dictionary = {}
var _world: VoxelWorld
var _linger_spheres := PackedVector3Array()

func _initialize() -> void:
	call_deferred("capture")

func ground_under(w: VoxelWorld, x: float, z: float) -> Vector3:
	var hit: Dictionary = w.raycast(Vector3(x, 400.0, z), Vector3.DOWN, 800.0)
	return hit["pos"] if hit["hit"] else Vector3(x, 0.0, z)

# The flattest of eight compass bearings: every one of them must still see ground out at
# 120 m, or a patch placed along it is hidden and the case proves nothing.
func open_bearing(w: VoxelWorld, from: Vector3) -> Vector3:
	var best := Vector3.FORWARD
	var best_score := -1.0
	for i in range(8):
		var dir := Vector3(sin(TAU * i / 8.0), 0.0, -cos(TAU * i / 8.0))
		var score := 0.0
		for d in [30.0, 50.0, 80.0, 120.0]:
			var hit: Dictionary = w.raycast(from, dir, 400.0)
			score += 1.0 if hit["hit"] and float(hit["distance"]) >= d else 0.0
		if score > best_score:
			best_score = score
			best = dir
	return best

func capture() -> void:
	var out := "/tmp/transparency-capture"
	var case_name := "seam"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
		if arg.begins_with("--case="):
			case_name = arg.trim_prefix("--case=")
	DirAccess.make_dir_recursive_absolute(out)
	var scene: Node = load("res://demo/main.tscn").instantiate()
	root.add_child(scene)
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	_world = world
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	# Still wind, so two runs of the same case are one image.
	world.set_grass_value("wind_strength", 0.0)
	world.set_grass_value("wind_speed", 0.0)
	if not world.hooks().debug_init_physics():
		push_error("transparency capture could not initialize the mesh worker")
		quit(1)
		return
	var ice := 0
	for m in world.material_table():
		if m["name"] == "ice":
			ice = m["id"]
	if ice <= 0:
		push_error("transparency capture: no ice material in the table")
		quit(1)
		return
	var camera: Camera3D = player.get_node("Camera3D")
	camera.far = 3000.0
	scene.get_node("HUD").visible = false

	# The player owns the camera, so the pose is set on the player and the camera is given a
	# local offset, exactly as demo/benchmark.gd and demo/capture.gd both do: setting the
	# camera's global transform first and the player's after leaves the two disagreeing.
	var home := ground_under(world, 20.0, 30.0)
	var cam := home + Vector3(0.0, EYE, 0.0)
	var dir := open_bearing(world, cam)
	player.global_transform = Transform3D(Basis.IDENTITY, cam - Vector3(0.0, 1.6, 0.0))
	camera.transform = Transform3D(Basis.looking_at(dir, Vector3.UP), Vector3(0.0, 1.6, 0.0))
	print("TRANSPARENCY_CAPTURE pose cam=%s fwd=%s" % [
		str(camera.global_position), str(-camera.global_transform.basis.z)])
	var band: Vector2 = world.hooks().debug_lod_fade_band()
	var placed := PackedStringArray()
	if case_name == "seam":
		# Three patches at true ranges that straddle the band the report printed above, and a
		# camera high enough to see all three over whatever the ground puts between them. The
		# height is searched rather than fixed: too low and a ridge hides the near patch, too
		# high and the frame is a plan view with no seam in it.
		var ranges := [band.x * 0.7, (band.x + band.y) * 0.5, band.y * 1.6]
		var heights := [45.0, 70.0, 100.0, 140.0]
		for high in heights:
			var eye := home + Vector3(0.0, high, 0.0)
			var spots := PackedVector3Array()
			for d: float in ranges:
				var best_p := Vector3.ZERO
				var best_err := 1.0e9
				for x in range(5, 400):
					var p := ground_under(world, eye.x + dir.x * x, eye.z + dir.z * x)
					var err: float = absf(eye.distance_to(p) - d)
					if err < best_err:
						best_err = err
						best_p = p
				# The centre of a 7 m ball can sit behind a ridge while the ball itself stands
				# over it, so what has to be visible is the top of the shell, not its middle.
				var probe := best_p + Vector3(0.0, 5.0, 0.0)
				var seen: Dictionary = world.raycast(eye, (probe - eye).normalized(), 400.0)
				if seen["hit"] and float(seen["distance"]) < eye.distance_to(probe) - 2.0:
					spots = PackedVector3Array()
					break
				spots.append(best_p)
			if spots.size() < ranges.size():
				continue
			cam = eye
			player.global_transform = Transform3D(Basis.IDENTITY, home)
			camera.transform = Transform3D(Basis.looking_at(dir, Vector3.UP), Vector3(0.0, high, 0.0))
			var aim_point := ground_under(world, eye.x + dir.x * 90.0, eye.z + dir.z * 90.0)
			camera.look_at(cam + (aim_point - cam).normalized())
			for p in spots:
				world.hooks().debug_apply_sphere_add(p, 7.0, ice)
				placed.append("%.0fm@%s" % [cam.distance_to(p), p])
			# Three separate balls each cross the band on their own edge, which is not the
			# question. This chain is ONE ice ridge running from inside the near field to past
			# the far horizon, so the band crosses it in the middle of the frame and the two
			# renderers have to hand over along the same surface.
			for x in range(20, 150, 6):
				var p := ground_under(world, cam.x + dir.x * x, cam.z + dir.z * x)
				world.hooks().debug_apply_sphere_add(p, 5.0, ice)
			placed.append("chain r=5 every 6m from 20m to 146m")
			break
		if placed.is_empty():
			# A capture with no ice in it proves nothing, so this is a failure like the rest of
			# the file's, not a note in the log: no PNG, non-zero exit.
			push_error("transparency capture: no camera height of %s sees all three patches" % str(heights))
			quit(1)
			return
	elif case_name == "foliage":
		var p := ground_under(world, cam.x + dir.x * 14.0, cam.z + dir.z * 14.0)
		world.hooks().debug_apply_sphere_add(p, 4.0, ice)
		placed.append("14m@%s" % p)
	elif case_name == "inside":
		var p := ground_under(world, cam.x + dir.x * 7.0, cam.z + dir.z * 7.0)
		world.hooks().debug_apply_sphere_add(p, 6.0, ice)
		placed.append("inside r=6 @%s" % p)
		player.global_transform = Transform3D(Basis.IDENTITY, p - Vector3(0.0, 1.6, 0.0))
		camera.transform = Transform3D(Basis.looking_at(dir, Vector3.UP), Vector3(0.0, 1.6, 0.0))
		cam = p + Vector3(0.0, EYE, 0.0)
	elif case_name == "sky":
		# Ground under a point 110 m out, lifted clear of it: nothing opaque behind the ball
		# but sky, so whatever the composite returns there is the sky through the ice.
		var p := ground_under(world, cam.x + dir.x * 110.0, cam.z + dir.z * 110.0) \
				+ Vector3(0.0, 45.0, 0.0)
		world.hooks().debug_apply_sphere_add(p, 7.0, ice)
		placed.append("sky r=7 @%s range=%.0fm" % [p, cam.distance_to(p)])
		camera.look_at(cam + (p - cam).normalized(), Vector3.UP)
	elif case_name == "linger":
		# A chain, not a ball: a phantom outline has to have a silhouette to survive in.
		_linger_spheres = PackedVector3Array()
		for x in range(14, 110, 8):
			var p := ground_under(world, cam.x + dir.x * x, cam.z + dir.z * x)
			world.hooks().debug_apply_sphere_add(p, 6.0, ice)
			_linger_spheres.append(p)
		placed.append("chain r=6 every 8m from 14m to %dm" % (_linger_spheres.size() * 8 + 6))
	else:
		# A typo'd --case= must not fall through to the foliage frame: it would save a
		# perfectly good PNG of the wrong thing and exit 0. Same standard as every other
		# failure in this file.
		push_error("transparency capture: unknown --case=%s (seam, foliage, inside, sky, linger)"
				% case_name)
		quit(1)
		return
	print("TRANSPARENCY_CAPTURE case=%s cam=%s dir=%s fade_band=%.1f..%.1f ice=%s" % [
		case_name, cam, dir, band.x, band.y, str(placed)])

	if not await settle():
		push_error("transparency capture did not settle: %s" % _stats)
		quit(1)
		return
	await save_png(out, case_name)
	if case_name == "linger":
		for c in _linger_spheres:
			world.hooks().debug_apply_sphere_subtract(c, 6.0)
		_linger_spheres = PackedVector3Array()
		if not await settle():
			push_error("transparency capture did not re-settle: %s" % _stats)
			quit(1)
			return
		await save_png(out, "linger_after")
	world.shutdown_render_resources()
	await process_frame
	quit()

# World quiet with the whole LoD tree drawn. False on failure, with _stats set for the log.
func settle() -> bool:
	var quiet := 0
	for frame in range(6000):
		await process_frame
		_stats = _world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(_stats.get("requests_pending", 1)) == 0 \
			and int(_stats.get("builds_in_flight", 1)) == 0 else 0
		if quiet >= 30 and int(_stats.get("draw_pages", 0)) > 0:
			return true
	return false

func save_png(out: String, name: String) -> void:
	for frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	var path := out.path_join("%s.png" % name)
	var img := root.get_texture().get_image()
	var err := img.save_png(path)
	print("TRANSPARENCY_CAPTURE saved=%s err=%d size=%dx%d chunks=%s draw_pages=%s pages_used=%s shell_pages=%s grass=%s" % [
		path, err, img.get_width(), img.get_height(),
		str(_stats.get("chunks_resident", -1)), str(_stats.get("draw_pages", -1)),
		str(_stats.get("pages_used", -1)), str(_stats.get("shell_pages", -1)),
		str((_world.hooks().debug_grass_stats() as Dictionary).get("blades", -1))])
