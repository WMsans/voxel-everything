extends SceneTree
# Reproducible look check for the title flow and each world type, through the shipping path:
# the same CreateWorld.build_world the Create button calls, then GroundSpawn, then a frame.
# godot --path . --resolution 1280x720 -s res://tools/world_type_capture.gd -- --out=/tmp/world-types

const CreateWorld := preload("res://demo/scripts/create_world.gd")

func _initialize() -> void:
	call_deferred("capture")

func _shot(path: String) -> void:
	await RenderingServer.frame_post_draw
	root.get_texture().get_image().save_png(path)
	print("WORLD_TYPE_CAPTURE saved ", path)

func capture() -> void:
	var out := "/tmp/world-types"
	var only := ""
	var seed_text := "hello"
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
		if arg.begins_with("--type="):
			only = arg.trim_prefix("--type=")
		if arg.begins_with("--seed="):
			seed_text = arg.trim_prefix("--seed=")
	DirAccess.make_dir_recursive_absolute(out)

	for scene_path in ["res://demo/scenes/title.tscn", "res://demo/scenes/create_world.tscn"]:
		var menu: Node = load(scene_path).instantiate()
		root.add_child(menu)
		for i in range(10):
			await process_frame
		await _shot(out.path_join(scene_path.get_file().get_basename() + ".png"))
		menu.free()

	for t in CreateWorld.load_types("res://demo/world_types"):
		if only != "" and t.display_name != only:
			continue
		var main: Node = CreateWorld.build_world(t, CreateWorld.parse_seed(seed_text))
		root.add_child(main)
		var world: VoxelWorld = main.get_node("VoxelWorld")
		var player: Node3D = main.get_node("Player")
		player.set_physics_process(false)
		player.set_process_unhandled_input(false)
		var settled := false
		for frame in range(6000):
			await process_frame
			if main.get_node_or_null("GroundSpawn") != null:
				continue
			var st: Dictionary = world.hooks().debug_lod_stats()
			if int(st.get("requests_pending", 1)) == 0 and int(st.get("builds_in_flight", 1)) == 0 \
					and int(st.get("draw_pages", 0)) > 0:
				settled = true
				break
		if not settled:
			push_error("world type capture did not settle for %s" % t.display_name)
		var cam: Camera3D = player.get_node("Camera3D")
		cam.rotation.x = -0.25
		for i in range(8):
			await process_frame
		await _shot(out.path_join("world_%s.png" % t.display_name.to_lower()))
		print("WORLD_TYPE_CAPTURE ", t.display_name, " seed ", world.world_seed,
				" player ", player.global_position)
		world.shutdown_render_resources()
		await process_frame
		main.free()
		await process_frame
	quit()
