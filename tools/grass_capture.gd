extends SceneTree
# Reproducible, GPU-backed grass look check through the SHIPPING render path -- not the
# debug hooks, which re-implement it. Run with:
# godot --path . --resolution 1280x720 -s res://tools/grass_capture.gd -- --out=/tmp/grass
#
# --rock=radius,distance floats a rock sphere `distance` metres up-sun of the --at spot, so
# a frame over open rolling grass can show whether blades take the terrain's shadow.
#
# Places the camera low over the grass terrain the grass suites stream around, which is the
# angle the BotW reference is shot from and the one that shows bare ground if there is any.

func _initialize() -> void:
	call_deferred("capture")

func capture() -> void:
	var out := "/tmp/grass-capture"
	var height := 1.4
	var pitch := -0.30
	var look := Vector3.ZERO
	var at := Vector3(30.0, 56.2, 30.0)
	var rock := Vector2.ZERO
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
		if arg.begins_with("--height="):
			height = float(arg.trim_prefix("--height="))
		if arg.begins_with("--pitch="):
			pitch = float(arg.trim_prefix("--pitch="))
		if arg.begins_with("--dir="):
			var parts := arg.trim_prefix("--dir=").split(",")
			look = Vector3(float(parts[0]), float(parts[1]), float(parts[2]))
		if arg.begins_with("--at="):
			var a := arg.trim_prefix("--at=").split(",")
			at = Vector3(float(a[0]), float(a[1]), float(a[2]))
		if arg.begins_with("--rock="):
			var r := arg.trim_prefix("--rock=").split(",")
			rock = Vector2(float(r[0]), float(r[1]))
	DirAccess.make_dir_recursive_absolute(out)
	var scene: Node = load("res://demo/main.tscn").instantiate()
	root.add_child(scene)
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	if not world.hooks().debug_init_physics():
		push_error("grass capture could not initialize the mesh worker")
		quit(1)
		return
	if rock.x > 0.0:
		# DirectionalLight3D emits along its local -Z, so +Z is toward the sun (ve::SunState).
		var light: DirectionalLight3D = scene.get_node("DirectionalLight3D")
		var sun_dir := light.global_transform.basis.z.normalized()
		world.hooks().debug_apply_sphere_add(at + sun_dir * rock.y, rock.x, 2)
	var camera: Camera3D = player.get_node("Camera3D")
	# The same spot every grass suite streams around, at eye height over the ground.
	player.global_position = at + Vector3(0.0, height, 0.0)
	camera.global_position = player.global_position
	var dir := look if look != Vector3.ZERO else Vector3(0.6, pitch, -1.0)
	camera.look_at(camera.global_position + dir)
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
		push_error("grass capture did not settle: %s" % stats)
		quit(1)
		return
	for frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(out.path_join("grass.png"))
	print("GRASS_CAPTURE saved, grass stats: ", world.hooks().debug_grass_stats())
	world.shutdown_render_resources()
	await process_frame
	quit()
