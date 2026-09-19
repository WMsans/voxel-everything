extends SceneTree
# Reproducible, GPU-backed leaf look check through the SHIPPING render path -- not the
# debug hooks, which re-implement it. Run with:
# godot --path . --resolution 1280x720 -s res://tools/leaf_capture.gd -- --out=/tmp/leaf-capture
# (Not --headless: like every GPU suite here this needs a real RenderingDevice, and
# headless has none -- gdunit_tests.sh documents the same.)
#
# --leaf=key,value overrides one leaf setting before the world streams, repeatable -- the
# key,value override shape of tools/grass_capture.gd (grass_capture's flag for it is
# --grass=, and the same shape appears as --grass=key,value below, because the wind freeze
# touches grass knobs too). Not the benchmark's boolean --leaves=0|1.
# Example: --leaf=crown_shade,0 against --leaf=crown_shade,1 A/Bs the crown self-shade.
#
# --rock=radius,distance floats a rock sphere `distance` metres up-sun of the --at spot, so
# a frame over a grove can show whether cards take the terrain's shadow.
#
# Places the camera at (20, 60, 30) -- the same stream origin tests/test_leaves.gd uses, at
# crown height over the groves it streams. That view, not the usual hook view, is the one
# that sees canopies from the side.
#
# The capture is REPRODUCIBLE: wind is frozen on both systems (test_frame_contract's
# lesson: the grass blade's lean gust term is scaled by neither strength nor speed, only a
# gust field that has stopped scrolling drops the frame-counter time out of it; the leaf
# sway is gated by strength alone but freezes the same way) and SSGI is off, because its
# temporal accumulation rides the beauty frame counter. What no capture-side knob can pin
# is the shipped path's streaming ARRIVAL order -- the LoD page and grass brick draw lists
# come out of the atlas in chunk-completion order, so depth-tied seam pixels flip by a
# few hundred units of 1/255 run to run. Two runs of this tool differ by ~0.06% of pixels
# (max delta 74, all at card/page seams); the byte-comparison golden therefore does not
# live here. The machine-checked "the image did not change" gate is the grove camera in
# tests/test_frame_shipped_golden.gd, which pins tile means with the suite's tolerance
# idiom. A --leaf=wind_strength,X after the freeze un-pins the leaves deliberately.

func _initialize() -> void:
	call_deferred("capture")

func capture() -> void:
	var out := "/tmp/leaf-capture"
	var look := Vector3.ZERO
	var at := Vector3(20.0, 60.0, 30.0)
	var pitch := 0.05
	var rock := Vector2.ZERO
	var overrides := {}
	var grass_overrides := {}
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--leaf="):
			var kv := arg.trim_prefix("--leaf=").split(",")
			overrides[kv[0]] = float(kv[1])
		if arg.begins_with("--grass="):
			var gk := arg.trim_prefix("--grass=").split(",")
			grass_overrides[gk[0]] = float(gk[1])
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
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
	# The determinism freezes, applied before the overrides so the overrides stay the last
	# word. See the header comment for why each one is needed.
	world.set_effect_enabled("ssgi", false)
	world.set_grass_value("wind_strength", 0.0)
	world.set_grass_value("wind_speed", 0.0)
	world.set_leaf_value("wind_strength", 0.0)
	world.set_leaf_value("wind_speed", 0.0)
	for key in overrides:
		world.set_leaf_value(key, overrides[key])
	for key in grass_overrides:
		world.set_grass_value(key, grass_overrides[key])
	if not world.hooks().debug_init_physics():
		push_error("leaf capture could not initialize the mesh worker")
		quit(1)
		return
	if rock.x > 0.0:
		# DirectionalLight3D emits along its local -Z, so +Z is toward the sun (ve::SunState).
		var light: DirectionalLight3D = scene.get_node("DirectionalLight3D")
		var sun_dir := light.global_transform.basis.z.normalized()
		world.hooks().debug_apply_sphere_add(at + sun_dir * rock.y, rock.x, 2)
	var camera: Camera3D = player.get_node("Camera3D")
	# The same spot every leaf suite streams around, at crown height -- no eye offset.
	player.global_position = at
	camera.global_position = at
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
		push_error("leaf capture did not settle: %s" % stats)
		quit(1)
		return
	for frame in range(8):
		await process_frame
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(out.path_join("leaf.png"))
	print("LEAF_CAPTURE saved, leaf stats: ", world.hooks().debug_leaf_stats())
	world.shutdown_render_resources()
	await process_frame
	quit()
