extends GdUnitTestSuite

# GPU/CPU differential test for gradient mirrors (Task 4, spec section 8).
# shaders/field.glslh gradient helpers must agree with ve::eval_field_gradient.
#
# Output buffer: two vec4 records per point:
#   record 0 = sdf, material, gradient.x, gradient.y
#   record 1 = gradient.z, exact?1:0, 0, 0
# SDF tolerance: two encoded steps (same as test_field_diff.gd).
# Gradient: where both paths report exact and sample is >0.01 from any CSG tie,
# normalize gradients and require dot > 0.9999.

const SDF_STEP := 1.28 / 255.0
const MAX_STEPS := 2.0
const TIGHT_FRACTION := 0.99
const GRAD_TIE_EPSILON := 0.01
const GRAD_DOT_THRESHOLD := 0.9999
# Looser threshold for pairs where the CPU side is inexact: since the terrain pipeline
# landed, the CPU base gradient is finite differences (inexact by construction) while the
# GPU base gradient differentiates the same field with the same taps. Libm-level field
# noise is amplified by the 1/(2e) differentiation step, so demand directional agreement
# rather than analytic-grade equality.
const GRAD_DOT_THRESHOLD_INEXACT := 0.999

const OP_SUBTRACT := 0
const OP_ADD := 1
const OP_PAINT := 2
const OP_BOX_SUBTRACT := 3

var _world: VoxelWorld
var _rd: RenderingDevice

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	# Initialized: the gradient probe compiles the generated field override against the
	# pipeline CPU generator (see test_field_diff.gd).
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null

func make_op(type: int, material: int, pos: Vector3, radius: float, aux0: int = 0, aux1: int = 0) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(pos.x)
	b.put_float(pos.y)
	b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(aux0)
	b.put_u32(aux1)
	return b.data_array

func pack_extent3(nx: int, ny: int, nz: int) -> int:
	return nx | (ny << 10) | (nz << 20)

func box_op(lo: Vector3i, hi: Vector3i) -> PackedByteArray:
	return make_op(OP_BOX_SUBTRACT, 0, Vector3(lo) * 0.8, 0.0, pack_extent3(hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1), 0)

func hills(x: float, z: float) -> float:
	return 6.0 * sin(x * 0.11) * cos(z * 0.13) + 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043) + 1.0 * sin(x * 0.23 + z * 0.19)

func box_sdf(lo: Vector3, hi: Vector3, p: Vector3) -> float:
	var c := 0.5 * (lo + hi)
	var h := 0.5 * (hi - lo)
	var q := (p - c).abs() - h
	var outside := Vector3(maxf(q.x, 0.0), maxf(q.y, 0.0), maxf(q.z, 0.0)).length()
	var inside := minf(maxf(q.x, maxf(q.y, q.z)), 0.0)
	return outside + inside

func decode_u32_at(b: PackedByteArray, off: int) -> int:
	return b[off] | (b[off+1] << 8) | (b[off+2] << 16) | (b[off+3] << 24)

func decode_f32_at(b: PackedByteArray, off: int) -> float:
	var spb := StreamPeerBuffer.new()
	spb.big_endian = false
	spb.data_array = b.slice(off, off+4)
	spb.seek(0)
	return spb.get_float()

func estimate_tie_distance(p: Vector3, ops: PackedByteArray, op_count: int) -> float:
	var h := hills(p.x, p.z)
	var terrain_sdf := p.y - 51.2 - h
	var cx := 30.0; var cz := 30.0
	var cy := 51.2 + hills(cx, cz) - 2.0
	var cave_len := (p - Vector3(cx, cy, cz)).length()
	var cave := cave_len - 5.0
	var candidates: Array[float] = []
	candidates.append(terrain_sdf)
	candidates.append(-cave)
	for i in range(op_count):
		var off := i * 32
		var type := decode_u32_at(ops, off)
		var pos := Vector3(decode_f32_at(ops, off+8), decode_f32_at(ops, off+12), decode_f32_at(ops, off+16))
		var radius := decode_f32_at(ops, off+20)
		var aux0 := decode_u32_at(ops, off+24)
		if type == OP_SUBTRACT or type == OP_ADD or type == OP_PAINT:
			var sp := (p - pos).length() - radius
			if type == OP_SUBTRACT:
				candidates.append(-sp)
			else:
				candidates.append(sp)
		elif type == OP_BOX_SUBTRACT:
			var nx := float(aux0 & 0x3FF)
			var ny := float((aux0 >> 10) & 0x3FF)
			var nz := float((aux0 >> 20) & 0x3FF)
			var lo := pos
			var hi := pos + Vector3(nx, ny, nz) * 0.8
			var m := maxf(radius, 0.0)
			var bd := box_sdf(lo - Vector3(m, m, m), hi + Vector3(m, m, m), p)
			candidates.append(-bd)
	# Winning SDF is max/base or min/max per op order - approximate by taking final max/min.
	# Instead estimate tie as difference between best and second-best candidate values when ranking by final evaluation.
	# For simplicity, use smallest absolute difference between any two candidates as tie proximity.
	var min_diff := 1e30
	for i in range(candidates.size()):
		for j in range(i+1, candidates.size()):
			var d := absf(candidates[i] - candidates[j])
			if d < min_diff:
				min_diff = d
	return min_diff

func sample_points() -> PackedVector3Array:
	var pts := PackedVector3Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260813
	for i in range(512):
		pts.append(Vector3(rng.randf_range(-20.0, 60.0), rng.randf_range(21.2, 81.2), rng.randf_range(-20.0, 60.0)))
	for i in range(128):
		pts.append(Vector3(rng.randf_range(700.0, 900.0), rng.randf_range(11.2, 71.2), rng.randf_range(700.0, 900.0)))
	return pts

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
	var code: String = _world.hooks().debug_load_shader("res://shaders/field_gradient_probe.comp.glsl")
	assert_str(code).is_not_empty()
	code = code.replace("#[compute]\n", "")

	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = code
	var spirv := _rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := _rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()

	var op_bytes := ops.duplicate()
	if op_bytes.size() < 32:
		op_bytes.resize(32)
	var op_buf := _rd.storage_buffer_create(op_bytes.size(), op_bytes)

	var pt_bytes := PackedFloat32Array()
	for p in pts:
		pt_bytes.append_array(PackedFloat32Array([p.x, p.y, p.z, 0.0]))
	var pt_buf := _rd.storage_buffer_create(pt_bytes.size() * 4, pt_bytes.to_byte_array())
	var out_buf := _rd.storage_buffer_create(pts.size() * 32)
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

func compare(pts: PackedVector3Array, ops: PackedByteArray, op_count: int, label: String) -> void:
	var gpu := run_gpu(pts, ops, op_count)
	assert_int(gpu.size()).is_equal(pts.size() * 8)

	var worst := 0.0
	var within_one := 0
	var grad_compared := 0
	var grad_mismatched := 0
	var worst_dot := 1.0
	var fdiff_compared := 0
	var fdiff_mismatched := 0
	var worst_fdiff_dot := 1.0
	for i in range(pts.size()):
		var cpu: Dictionary = _world.hooks().debug_eval_field_gradient(pts[i], ops, op_count)
		var cpu_sdf: float = cpu["sdf"]
		var cpu_mat: int = int(cpu["material"])
		var cpu_grad: Vector3 = cpu["gradient"]
		var cpu_exact: bool = bool(cpu["exact"])

		var gpu_sdf: float = gpu[i * 8 + 0]
		var gpu_mat: int = int(gpu[i * 8 + 1])
		var gpu_grad := Vector3(gpu[i * 8 + 2], gpu[i * 8 + 3], gpu[i * 8 + 4])
		var gpu_exact: bool = gpu[i * 8 + 5] > 0.5

		# Material is part of the mirrored contract, not decoration: the CPU volume-add
		# gradient branch used to overwrite it wholesale while GLSL kept apply_op's rule.
		if cpu_sdf <= 0.0 and gpu_sdf <= 0.0:
			assert_int(gpu_mat).override_failure_message(
				"%s: material differs at %s (cpu %d, gpu %d)" % [label, pts[i], cpu_mat, gpu_mat]
				).is_equal(cpu_mat)

		var diff: float = absf(gpu_sdf - cpu_sdf) / SDF_STEP
		worst = maxf(worst, diff)
		if diff <= 1.0:
			within_one += 1

		if cpu_exact and gpu_exact:
			var tie := estimate_tie_distance(pts[i], ops, op_count)
			if tie <= GRAD_TIE_EPSILON:
				continue
			var cpu_len := cpu_grad.length()
			var gpu_len := gpu_grad.length()
			if cpu_len > 1e-6 and gpu_len > 1e-6:
				var cn := cpu_grad / cpu_len
				var gn := gpu_grad / gpu_len
				var dot: float = cn.dot(gn)
				worst_dot = minf(worst_dot, dot)
				grad_compared += 1
				if dot <= GRAD_DOT_THRESHOLD:
					grad_mismatched += 1
		elif gpu_exact and not cpu_exact:
			# Pipeline-CPU base gradient (finite differences, inexact) against the GPU
			# base gradient (same taps, same field): directional agreement only.
			var tie := estimate_tie_distance(pts[i], ops, op_count)
			if tie <= GRAD_TIE_EPSILON:
				continue
			var cpu_len := cpu_grad.length()
			var gpu_len := gpu_grad.length()
			if cpu_len > 1e-6 and gpu_len > 1e-6:
				var cn := cpu_grad / cpu_len
				var gn := gpu_grad / gpu_len
				var dot: float = cn.dot(gn)
				worst_fdiff_dot = minf(worst_fdiff_dot, dot)
				fdiff_compared += 1
				if dot <= GRAD_DOT_THRESHOLD_INEXACT:
					fdiff_mismatched += 1

	assert_float(worst).override_failure_message(
		"%s: worst sdf disagreement %.2f encoded steps" % [label, worst]).is_less(MAX_STEPS)
	assert_float(float(within_one) / float(pts.size())).override_failure_message(
		"%s: only %d/%d samples within one encoded step" % [label, within_one, pts.size()]
		).is_greater(TIGHT_FRACTION)
	assert_int(grad_compared + fdiff_compared).override_failure_message(
		"%s: no gradient pair was comparable, so the assertion below proves nothing" % label
		).is_greater(0)
	if grad_compared > 0:
		assert_int(grad_mismatched).override_failure_message(
			"%s: %d/%d gradient mismatches (worst dot %.5f)" % [label, grad_mismatched, grad_compared, worst_dot]).is_equal(0)
	if fdiff_compared > 0:
		assert_int(fdiff_mismatched).override_failure_message(
			"%s: %d/%d finite-difference gradient mismatches (worst dot %.5f)" % [label, fdiff_mismatched, fdiff_compared, worst_fdiff_dot]).is_equal(0)

func test_base_field_matches_the_cpu_generator() -> void:
	compare(sample_points(), PackedByteArray(), 0, "base")

func test_sphere_subtract_matches() -> void:
	var ops := make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0)
	compare(sample_points(), ops, 1, "subtract")

func test_sphere_add_matches() -> void:
	var ops := make_op(OP_ADD, 4, Vector3(10.0, 56.2, 10.0), 6.0)
	compare(sample_points(), ops, 1, "add")

func test_sphere_paint_matches() -> void:
	var ops := make_op(OP_PAINT, 2, Vector3(10.0, 49.2, 10.0), 8.0)
	compare(sample_points(), ops, 1, "paint")

func test_box_subtract_matches() -> void:
	var pts := sample_points()
	var ops := box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7))
	compare(pts, ops, 1, "box")

func test_ordered_op_chain_matches() -> void:
	var ops := make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 8.0)
	ops.append_array(make_op(OP_ADD, 4, Vector3(10.0, 51.2, 10.0), 4.0))
	ops.append_array(make_op(OP_PAINT, 3, Vector3(12.0, 51.2, 12.0), 5.0))
	compare(sample_points(), ops, 3, "chain")

func test_ordered_chain_with_box_matches() -> void:
	var ops := make_op(OP_SUBTRACT, 0, Vector3(8.0, 51.2, 8.0), 4.0)
	ops.append_array(box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7)))
	ops.append_array(make_op(OP_ADD, 4, Vector3(10.0, 56.2, 10.0), 6.0))
	compare(sample_points(), ops, 3, "chain+box")
