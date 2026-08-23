extends GdUnitTestSuite
const CEL_SHADER := preload("res://shaders/cel_object.gdshader")
var _nodes: Array = []
func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n): n.free()
	_nodes.clear()
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true; w.physics_enabled = false
	add_child(w); _nodes.append(w); return w
func make_probe() -> Dictionary:
	var vp := SubViewport.new()
	vp.size = Vector2i(16, 16)
	vp.render_target_clear_mode = SubViewport.CLEAR_MODE_ALWAYS
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	vp.use_hdr_2d = true
	add_child(vp); _nodes.append(vp)
	var cam := Camera3D.new(); cam.projection = Camera3D.PROJECTION_ORTHOGONAL
	cam.size = 2.0; cam.position = Vector3(0, 0, 2); vp.add_child(cam); cam.current = true
	var mesh := MeshInstance3D.new(); var quad := QuadMesh.new(); quad.size = Vector2(2, 2)
	mesh.mesh = quad
	var mat := ShaderMaterial.new(); mat.shader = CEL_SHADER
	mat.set_shader_parameter("probe_mode", true); mesh.material_override = mat; vp.add_child(mesh)
	return {"viewport": vp, "material": mat}
func render_probe(probe: Dictionary, c: Array) -> Color:
	var m: ShaderMaterial = probe["material"]
	m.set_shader_parameter("probe_albedo", Vector3(c[0].r, c[0].g, c[0].b))
	m.set_shader_parameter("probe_ambient", Vector3(c[1].r, c[1].g, c[1].b))
	for p in [["probe_ndl",2],["probe_ndv",3],["probe_ndh",4],["probe_shadow",5],
			["probe_ao",6],["probe_gloss",7]]: m.set_shader_parameter(p[0], c[p[1]])
	var vp: SubViewport = probe["viewport"]; vp.render_target_update_mode = SubViewport.UPDATE_ONCE
	await RenderingServer.frame_post_draw
	return vp.get_texture().get_image().get_pixel(8, 8)
func test_shaderlanguage_matches_ve_cel_shade() -> void:
	var w := make_world(); var probe := make_probe()
	var cases := [
		[Color(.8,.2,.1),Color(0,0,0),1.0,1.0,0.0,1.0,1.0,0.0],
		[Color(.36,.55,.22),Color(.16,.19,.26),.079,.8,.3,1.0,1.0,0.0],
		[Color(.36,.55,.22),Color(.16,.19,.26),.081,.8,.3,1.0,1.0,0.0],
		[Color(.45,.42,.40),Color(.16,.19,.26),.319,.4,.9,1.0,1.0,.9],
		[Color(.45,.42,.40),Color(.16,.19,.26),.321,.4,.9,1.0,1.0,.9],
		[Color(.5,.5,.5),Color(.2,.2,.2),.659,.5,.71,.5,.4,.6],
		[Color(.5,.5,.5),Color(.2,.2,.2),.661,0.0,.73,.5,1.0,1.0],
		[Color(.02,.02,.9),Color(0,0,0),-2.0,1.0,0.0,0.0,1.0,0.0]]
	for c in cases:
		var got: Color = await render_probe(probe, c)
		var ref: Color = w.hooks().debug_cel_reference(c[0],c[1],c[2],c[3],c[4],c[5],c[6],c[7])
		assert_float(absf(got.r-ref.r)).is_less(0.006)
		assert_float(absf(got.g-ref.g)).is_less(0.006)
		assert_float(absf(got.b-ref.b)).is_less(0.006)
func test_demo_cube_uses_the_shared_shader() -> void:
	var scene: Node = load("res://demo/main.tscn").instantiate()
	# Demo helper scripts use absolute /root/Main paths; remove them for this resource-only check.
	scene.get_node("HUD/Label").set_script(null)
	scene.get_node("EditTool").set_script(null)
	add_child(scene); _nodes.append(scene)
	var mat := (scene.get_node("TestCube") as MeshInstance3D).material_override as ShaderMaterial
	assert_object(mat).is_not_null()
	assert_str(mat.shader.resource_path).is_equal("res://shaders/cel_object.gdshader")
