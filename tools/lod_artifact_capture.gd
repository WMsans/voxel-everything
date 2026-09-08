extends SceneTree
# Reproducible, GPU-backed diagnostic. Run with:
# godot --path . --resolution 1280x720 -s res://tools/lod_artifact_capture.gd -- --out=/tmp/lod-before

func _initialize() -> void:
	call_deferred("capture")

func capture() -> void:
	var out := "/tmp/lod-artifacts"
	var diagnostic := ""
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with("--out="):
			out = arg.trim_prefix("--out=")
		if arg.begins_with("--diagnostic="):
			diagnostic = arg.trim_prefix("--diagnostic=")
	DirAccess.make_dir_recursive_absolute(out)
	var scene: Node = load("res://demo/main.tscn").instantiate()
	root.add_child(scene)
	var world: VoxelWorld = scene.get_node("VoxelWorld")
	# In-memory diagnostic variants only; production shader files remain untouched.
	if diagnostic == "same-shadow-cut":
		world.sun_cascade_min_level = false
	if diagnostic == "no-skirt-shadows":
		var shadow := FileAccess.get_file_as_string("res://shaders/lod_shadow.vert.glsl")
		shadow = shadow.replace("vec3 p = lod_corner_pos", "if (lod_bits_get(w, 94, 1) != 0u) { gl_Position = vec4(2.0, 2.0, 2.0, 1.0); return; } vec3 p = lod_corner_pos")
		world.hooks().debug_set_shader_override("lod_shadow.vert.glsl", shadow)
	if diagnostic in ["outline-depth", "outline-normal"]:
		var code := FileAccess.get_file_as_string("res://shaders/outline.comp.glsl")
		if diagnostic == "outline-depth":
			code = code.replace("return a.kind != 0 && a.kind == b.kind && 1.0 - dot(a.n, b.n) > pc.params.y;", "return false;")
		else:
			code = code.replace("if (b.depth <= 0.0) return !b.solid && background_continues(a, px, step);", "if (b.depth <= 0.0) return false;")
			code = code.replace("if (depth_break(a, b)) return true;", "")
		world.hooks().debug_set_shader_override("outline.comp.glsl", code)
	if diagnostic in ["shadow-bias8", "shadow-pcf"]:
		var deferred := FileAccess.get_file_as_string("res://shaders/deferred.comp.glsl")
		if diagnostic == "shadow-bias8":
			deferred = deferred.replace("4.0 + 4.0 * slope", "8.0 + 8.0 * slope")
		else:
			deferred = deferred.replace("return (p.z + bias >= texture(sun_map, vec3(uv, float(c))).r) ? 1.0 : 0.0;", "float visible = 0.0; vec2 duv = 1.0 / vec2(textureSize(sun_map, 0).xy); for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) visible += (p.z + bias >= texture(sun_map, vec3(uv + vec2(x,y) * duv, float(c))).r) ? 1.0 : 0.0; return visible / 9.0;")
		world.hooks().debug_set_shader_override("deferred.comp.glsl", deferred)
	# "rim-ungated" is the pre-fix shader exactly: the cel rim driven by grazing angle alone,
	# which on open ground is a proxy for distance and paints the far field white. "rim-gate"
	# renders the gate itself, so the band it fires on can be seen rather than argued about.
	if diagnostic in ["rim-ungated", "rim-gate"]:
		var deferred := FileAccess.get_file_as_string("res://shaders/deferred.comp.glsl")
		if diagnostic == "rim-ungated":
			deferred = deferred.replace("silhouette_gate(px, size)", "1.0")
		else:
			deferred = deferred.replace("\tvec3 lit = cel_shade(",
				"\timageStore(out_lit, px, vec4(sil, sil, sil, 1.0)); return;\n\tvec3 lit = cel_shade(")
		world.hooks().debug_set_shader_override("deferred.comp.glsl", deferred)
	if diagnostic in ["albedo", "normals", "levels", "ribbons"]:
		var deferred := FileAccess.get_file_as_string("res://shaders/deferred.comp.glsl")
		deferred = deferred.replace("uint mat = uint(g1.z + 0.5);", "imageStore(out_lit, px, vec4(g0.rgb, 1.0)); return; uint mat = uint(g1.z + 0.5);")
		world.hooks().debug_set_shader_override("deferred.comp.glsl", deferred)
		if diagnostic != "albedo":
			var frag := FileAccess.get_file_as_string("res://shaders/lod.frag.glsl")
			if diagnostic == "normals":
				frag = frag.replace("surf.rgb * mix(1.0, props.y, 0.65)", "v_normal * 0.5 + 0.5")
			else:
				var vert := FileAccess.get_file_as_string("res://shaders/lod.vert.glsl")
				vert = vert.replace("void main() {", "layout(location = 3) out flat float v_level; void main() {")
				if diagnostic == "levels":
					vert = vert.replace("v_material = lod_bits_get(w, 78, 16);", "v_material = lod_bits_get(w, 78, 16); v_level = float(floatBitsToUint(chunks.v[ci * 2u + 1u].x));")
				else:
					vert = vert.replace("v_material = lod_bits_get(w, 78, 16);", "v_material = lod_bits_get(w, 78, 16); v_level = float(lod_bits_get(w, 94, 1));")
				world.hooks().debug_set_shader_override("lod.vert.glsl", vert)
				frag = frag.replace("void main() {", "layout(location = 3) in flat float v_level; void main() {")
				var diagnostic_color := "vec3(v_level / 7.0)" if diagnostic == "levels" else "vec3(v_level, 0.0, 0.0)"
				frag = frag.replace("surf.rgb * mix(1.0, props.y, 0.65)", diagnostic_color)
			world.hooks().debug_set_shader_override("lod.frag.glsl", frag)
	var player: CharacterBody3D = scene.get_node("Player")
	player.set_physics_process(false)
	player.set_process_unhandled_input(false)
	world.physics_enabled = false
	if not world.hooks().debug_init_physics():
		push_error("LOD capture could not initialize the mesh worker")
		quit(1)
		return
	var camera: Camera3D = player.get_node("Camera3D")
	player.global_position = Vector3(500, 320, 500)
	camera.global_position = player.global_position
	camera.look_at(camera.global_position + Vector3(0.1, -0.23, -1.0))
	camera.far = 6000.0
	scene.get_node("HUD").visible = false
	var quiet := 0
	var stats: Dictionary
	for frame in range(6000):
		await process_frame
		stats = world.hooks().debug_lod_stats()
		quiet = quiet + 1 if int(stats.get("requests_pending", 1)) == 0 and int(stats.get("builds_in_flight", 1)) == 0 else 0
		if frame % 300 == 0:
			print("LOD_CAPTURE frame=", frame, " pages=", stats.get("draw_pages", 0), " pending=", stats.get("lod_pending", -1))
		if quiet >= 30 and int(stats.get("draw_pages", 0)) > 0:
			break
	if quiet < 30:
		push_error("LOD capture did not settle: %s" % stats)
		quit(1)
		return
	stats.erase("draw_page_ids")
	stats.erase("resident_page_ids")
	print("LOD_CAPTURE settled: ", stats)
	var effects := ["outlines", "sun_shadow_map", "ssao", "ssgi", "ssr", "contact_shadows"]
	var defaults := {}
	for effect in effects:
		defaults[effect] = world.get_effect_enabled(effect)
	for mode in ["default", "no-outlines", "no-sun", "plain"]:
		for effect in effects:
			world.set_effect_enabled(effect, defaults[effect])
		if mode == "no-outlines": world.set_effect_enabled("outlines", false)
		if mode == "no-sun": world.set_effect_enabled("sun_shadow_map", false)
		if mode == "plain":
			for effect in effects: world.set_effect_enabled(effect, false)
		for frame in range(8): await process_frame
		await RenderingServer.frame_post_draw
		root.get_texture().get_image().save_png(out.path_join(mode + ".png"))
		print("LOD_CAPTURE saved ", mode)
	world.shutdown_render_resources()
	await process_frame
	quit()
