extends GdUnitTestSuite

# Characterization: the GPU field must match tests/golden/field_baseline.txt, the same corpus
# extension/tests/test_field_baseline.cpp pins the CPU against. test_field_diff.gd only proves
# the two agree with EACH OTHER, which both drifting together would satisfy.
#
# Tolerance is the established one: sin() is not bit-identical between glibc and a Vulkan
# driver, so the gate is expressed in encoded SDF steps.
const SDF_STEP := 1.28 / 255.0
const MAX_STEPS := 2.0
const TIGHT_FRACTION := 0.99

var _world: VoxelWorld
var _rd: RenderingDevice

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	# Initialized: this suite pins the generated pipeline field, not the fallback stub
	# (see test_field_diff.gd).
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null

func _bits_to_float(u: int) -> float:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(u)
	b.seek(0)
	return b.get_float()

func _load_golden() -> Dictionary:
	var f := FileAccess.open("res://tests/golden/field_baseline.txt", FileAccess.READ)
	assert_object(f).is_not_null()
	var pts := PackedVector3Array()
	var sdf := PackedFloat32Array()
	var mat := PackedInt32Array()
	while not f.eof_reached():
		var line := f.get_line().strip_edges()
		if line.is_empty() or line.begins_with("#"):
			continue
		var c := line.split(" ", false)
		assert_int(c.size()).is_equal(5)
		pts.append(Vector3(_bits_to_float(("0x" + c[0]).hex_to_int()),
			_bits_to_float(("0x" + c[1]).hex_to_int()),
			_bits_to_float(("0x" + c[2]).hex_to_int())))
		sdf.append(_bits_to_float(("0x" + c[3]).hex_to_int()))
		mat.append(int(c[4]))
	f.close()
	return {"pts": pts, "sdf": sdf, "mat": mat}

func _make_field_set(rd: RenderingDevice, shader: RID) -> RID:
	# Set 1, mirroring FieldContextSet: binding 0 params UBO, binding 1 sector map (one
	# int = -1, "no sector resident"). Plan A declares no sampled resources.
	var params: PackedByteArray = _world.hooks().debug_field_params_bytes()
	if params.is_empty():
		params.resize(16)
		params.fill(0)
	var ubo := rd.uniform_buffer_create(params.size(), params)
	var u0 := RDUniform.new()
	u0.uniform_type = RenderingDevice.UNIFORM_TYPE_UNIFORM_BUFFER
	u0.binding = 0
	u0.add_id(ubo)

	var map_bytes := PackedByteArray()
	map_bytes.resize(4)
	map_bytes.encode_s32(0, -1)
	var ssbo := rd.storage_buffer_create(map_bytes.size(), map_bytes)
	var u1 := RDUniform.new()
	u1.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	u1.binding = 1
	u1.add_id(ssbo)

	return rd.uniform_set_create([u0, u1], shader, 1)

func run_gpu(pts: PackedVector3Array, ops: PackedByteArray, op_count: int) -> PackedFloat32Array:
	var code: String = _world.hooks().debug_load_shader("res://shaders/field_probe.comp.glsl")
	assert_str(code).is_not_empty()
	# ve::load_shader_source keeps the Godot-only #[compute] annotation; glslang rejects it.
	code = code.replace("#[compute]\n", "")

	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = code
	var spirv := _rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := _rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()

	# The op pool must never be zero-sized even when there are no ops.
	var op_bytes := ops.duplicate()
	if op_bytes.size() < 32:
		op_bytes.resize(32)
	var op_buf := _rd.storage_buffer_create(op_bytes.size(), op_bytes)

	var pt_bytes := PackedFloat32Array()
	for p in pts:
		pt_bytes.append_array(PackedFloat32Array([p.x, p.y, p.z, 0.0]))
	var pt_buf := _rd.storage_buffer_create(pt_bytes.size() * 4, pt_bytes.to_byte_array())
	var out_buf := _rd.storage_buffer_create(pts.size() * 16)
	# field_probe.comp.glsl now also declares the volume pool at bindings 3 and 4;
	# sphere-op tests never touch those bytes, but the uniform set must cover every binding.
	var vsdf_buf := _rd.storage_buffer_create(4, PackedByteArray([0, 0, 0, 0]))
	var vmat_buf := _rd.storage_buffer_create(4, PackedByteArray([0, 0, 0, 0]))

	var uniforms := []
	for pair in [[0, op_buf], [1, pt_buf], [2, out_buf], [3, vsdf_buf], [4, vmat_buf]]:
		var u := RDUniform.new()
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		u.binding = pair[0]
		u.add_id(pair[1])
		uniforms.append(u)
	var uset := _rd.uniform_set_create(uniforms, shader, 0)
	var pipeline := _rd.compute_pipeline_create(shader)

	var push := PackedInt32Array([pts.size(), op_count, 0, 0]).to_byte_array()
	var list := _rd.compute_list_begin()
	_rd.compute_list_bind_compute_pipeline(list, pipeline)
	_rd.compute_list_bind_uniform_set(list, uset, 0)
	_rd.compute_list_bind_uniform_set(list, _make_field_set(_rd, shader), 1)
	_rd.compute_list_set_push_constant(list, push, push.size())
	_rd.compute_list_dispatch(list, (pts.size() + 63) / 64, 1, 1)
	_rd.compute_list_end()
	_rd.submit()
	_rd.sync()

	var out := _rd.buffer_get_data(out_buf).to_float32_array()
	_rd.free_rid(uset)
	_rd.free_rid(pipeline)
	_rd.free_rid(shader)
	_rd.free_rid(op_buf)
	_rd.free_rid(pt_buf)
	_rd.free_rid(out_buf)
	_rd.free_rid(vsdf_buf)
	_rd.free_rid(vmat_buf)
	return out

func test_gpu_field_matches_committed_baseline() -> void:
	var golden := _load_golden()
	var pts: PackedVector3Array = golden["pts"]
	assert_int(pts.size()).is_equal(640)

	# run_gpu() returns vec4 per point as (sdf, material, 0, 0), copied verbatim from
	# tests/test_field_diff.gd. Dispatched with an EMPTY op buffer and op_count 0, so
	# only the base field is exercised.
	var got: PackedFloat32Array = run_gpu(pts, PackedByteArray(), 0)
	assert_int(got.size()).is_equal(pts.size() * 4)

	var worst := 0.0
	var tight := 0
	for i in range(pts.size()):
		var d: float = absf(got[i * 4] - golden["sdf"][i]) / SDF_STEP
		worst = maxf(worst, d)
		if d <= 1.0:
			tight += 1
		assert_int(int(got[i * 4 + 1])).is_equal(golden["mat"][i])
	assert_float(worst).is_less_equal(MAX_STEPS)
	assert_float(float(tight) / float(pts.size())).is_greater_equal(TIGHT_FRACTION)
