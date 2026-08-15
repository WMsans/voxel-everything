extends GdUnitTestSuite

# GPU/CPU differential test for the field mirror (spec section 8). shaders/field.glslh must
# agree with ve::AnalyticGenerator + ve::apply_op or every brick the GPU generates is wrong.
#
# Tolerance: sin() is not bit-identical between glibc and a Vulkan driver, and the stored SDF
# is a uint8 with ~5 mm steps. A disagreement below half a step can never change a stored
# byte, so the gate is expressed in encoded steps rather than metres.
const SDF_STEP := 1.28 / 255.0        # metres per encoded step
const MAX_STEPS := 2.0                # no sample may differ by more than this
const TIGHT_FRACTION := 0.99          # ...and this share must be within one step

const OP_SUBTRACT := 0
const OP_ADD := 1
const OP_PAINT := 2

var _world: VoxelWorld
var _rd: RenderingDevice

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	# Byte-identical to ve::EditOp: type, material, pos[3], radius, pad[2] — 32 bytes.
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(pos.x)
	b.put_float(pos.y)
	b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0)
	b.put_u32(0)
	return b.data_array

func sample_points() -> PackedVector3Array:
	# A deterministic spread over the demo neighbourhood, the cave, and below the origin
	# plane, plus a few far-out points where sin() range reduction is hardest. The y ranges
	# are shifted +51.2 (the surface offset) so the samples cross the surface at
	# 51.2 +- 10 m and the carved cave (centre ~50.85 m) instead of the old surface at 0.
	var pts := PackedVector3Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260813
	for i in range(512):
		pts.append(Vector3(rng.randf_range(-20.0, 60.0), rng.randf_range(21.2, 81.2),
			rng.randf_range(-20.0, 60.0)))
	for i in range(128):
		pts.append(Vector3(rng.randf_range(700.0, 900.0), rng.randf_range(11.2, 71.2),
			rng.randf_range(700.0, 900.0)))
	return pts

func run_gpu(pts: PackedVector3Array, ops: PackedByteArray, op_count: int) -> PackedFloat32Array:
	var code: String = _world.debug_load_shader("res://shaders/field_probe.comp.glsl")
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

	var uniforms := []
	for pair in [[0, op_buf], [1, pt_buf], [2, out_buf]]:
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
	return out

func compare(pts: PackedVector3Array, ops: PackedByteArray, op_count: int, label: String) -> void:
	var gpu := run_gpu(pts, ops, op_count)
	assert_int(gpu.size()).is_equal(pts.size() * 4)

	var worst := 0.0
	var within_one := 0
	var mat_mismatch := 0
	var mat_compared := 0
	for i in range(pts.size()):
		var cpu: Vector2 = _world.debug_eval_field(pts[i], ops, op_count)
		var diff: float = absf(gpu[i * 4] - cpu.x) / SDF_STEP
		worst = maxf(worst, diff)
		if diff <= 1.0:
			within_one += 1
		# Material is a hard classification and flips across a band edge, so only compare
		# where the CPU sdf is far enough from 0 and from a band boundary for the tiny sdf
		# disagreement to be irrelevant.
		if absf(cpu.x) > 4.0 * SDF_STEP:
			mat_compared += 1
			if int(gpu[i * 4 + 1]) != int(cpu.y):
				mat_mismatch += 1

	assert_float(worst).override_failure_message(
		"%s: worst sdf disagreement %.2f encoded steps" % [label, worst]).is_less(MAX_STEPS)
	assert_float(float(within_one) / float(pts.size())).override_failure_message(
		"%s: only %d/%d samples within one encoded step" % [label, within_one, pts.size()]
		).is_greater(TIGHT_FRACTION)
	assert_int(mat_compared).is_greater(pts.size() / 2)
	assert_int(mat_mismatch).override_failure_message(
		"%s: %d material mismatches" % [label, mat_mismatch]).is_equal(0)

func test_base_field_matches_the_cpu_generator() -> void:
	compare(sample_points(), PackedByteArray(), 0, "base")

func test_sphere_subtract_matches() -> void:
	# Op centred on the new surface (51.2) so the samples actually pass through its sphere.
	var ops := make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0)
	compare(sample_points(), ops, 1, "subtract")

func test_sphere_add_matches() -> void:
	var ops := make_op(OP_ADD, 4, Vector3(10.0, 56.2, 10.0), 6.0)
	compare(sample_points(), ops, 1, "add")

func test_sphere_paint_matches() -> void:
	var ops := make_op(OP_PAINT, 2, Vector3(10.0, 49.2, 10.0), 8.0)
	compare(sample_points(), ops, 1, "paint")

func test_ordered_op_chain_matches() -> void:
	# Order matters: an add inside an earlier subtract must refill it on both sides.
	var ops := make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 8.0)
	ops.append_array(make_op(OP_ADD, 4, Vector3(10.0, 51.2, 10.0), 4.0))
	ops.append_array(make_op(OP_PAINT, 3, Vector3(12.0, 51.2, 12.0), 5.0))
	compare(sample_points(), ops, 3, "chain")
