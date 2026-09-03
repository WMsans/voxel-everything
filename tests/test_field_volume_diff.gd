extends GdUnitTestSuite

# GPU/CPU differential test for M4's two new op types (spec section 8). shaders/field.glslh
# must agree with ve::apply_op or a carve leaves a ghost of the island behind and a paste
# stamps a different rock than the one the physics dropped.
#
# Structure and tolerances are lifted from tests/test_field_diff.gd: sin() is not
# bit-identical between glibc and a Vulkan driver, and the stored SDF is a uint8 with ~5 mm
# steps, so the gate is expressed in encoded steps rather than metres. Box and volume ops
# involve no transcendentals at all, so in practice they agree far more tightly than the
# sphere ops do -- the gate stays where it is because the BASE field is still in the sum.
const SDF_STEP := 1.28 / 255.0
const MAX_STEPS := 2.0
const TIGHT_FRACTION := 0.99

const OP_SUBTRACT := 0
const OP_BOX_SUBTRACT := 3
const OP_VOLUME_ADD := 4

# The volume the tests paste. 16^3 keeps the GDScript that builds it instant; the shader
# reads the dimension out of the op, and slot 0's byte offset is zero whatever the pool's
# per-slot stride is, so a 16^3 buffer is a legal pool of one.
const VDIM := 16
const VVOXEL := 0.05
const VORIGIN := Vector3(10.0, 64.0, 10.0) # above the terrain everywhere
const VRADIUS := 0.25
const VMATERIAL := 2

var _world: VoxelWorld
var _rd: RenderingDevice
var _worlds: Array = []

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)
	_worlds.append(_world)
	# Initialized: the probes compile the generated field override against the pipeline
	# CPU generator. Uninitialized worlds silently test the stub against the built-in
	# generator instead (see test_field_diff.gd).
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	_rd = RenderingServer.create_local_rendering_device()

func after_test() -> void:
	if _rd != null:
		_rd.free()
		_rd = null
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func encode_sdf(d: float) -> int:
	# ve::encode_sdf: clamp to +-0.64 m, then 255 even steps.
	var t := clampf((d + 0.64) / 1.28, 0.0, 1.0)
	return int(floor(t * 255.0 + 0.5))

# A ball centred in its own lattice, as raw bytes for both sides.
func ball_volume() -> Array:
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	var c := 0.5 * float(VDIM - 1) * VVOXEL
	for z in range(VDIM):
		for y in range(VDIM):
			for x in range(VDIM):
				var p := Vector3(x, y, z) * VVOXEL - Vector3(c, c, c)
				var d := p.length() - VRADIUS
				var i := x + y * VDIM + z * VDIM * VDIM
				sdf[i] = encode_sdf(d)
				mat[i] = VMATERIAL if d <= 0.0 else 0
	return [sdf, mat]

func make_op(type: int, material: int, pos: Vector3, radius: float,
		aux0: int = 0, aux1: int = 0) -> PackedByteArray:
	# Byte-identical to ve::EditOp: type, material, pos[3], radius, aux[2] — 32 bytes.
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
	# ve::make_box_subtract: pos is the minimum corner in metres, aux[0] the cell extent.
	return make_op(OP_BOX_SUBTRACT, 0, Vector3(lo) * 0.8, 0.0,
		pack_extent3(hi.x - lo.x + 1, hi.y - lo.y + 1, hi.z - lo.z + 1), 0)

func volume_op(slot: int) -> PackedByteArray:
	return make_op(OP_VOLUME_ADD, 0, VORIGIN, VVOXEL, slot, VDIM)

func sample_points() -> PackedVector3Array:
	var pts := PackedVector3Array()
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260815
	# A dense cloud over the pasted volume's own extent, where the interesting disagreements
	# would be, ...
	for i in range(384):
		pts.append(VORIGIN + Vector3(rng.randf_range(-0.4, 1.2), rng.randf_range(-0.4, 1.2),
			rng.randf_range(-0.4, 1.2)))
	# ...and a spread over the terrain the box ops carve.
	for i in range(384):
		pts.append(Vector3(rng.randf_range(-4.0, 20.0), rng.randf_range(38.0, 62.0),
			rng.randf_range(-4.0, 20.0)))
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

func run_gpu(pts: PackedVector3Array, ops: PackedByteArray, op_count: int,
		vol: Array) -> PackedFloat32Array:
	var code: String = _world.hooks().debug_load_shader("res://shaders/field_probe.comp.glsl")
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
	var out_buf := _rd.storage_buffer_create(pts.size() * 16)
	var vsdf_buf := _rd.storage_buffer_create(vol[0].size(), vol[0])
	var vmat_buf := _rd.storage_buffer_create(vol[1].size(), vol[1])

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
	for rid in [uset, pipeline, shader, op_buf, pt_buf, out_buf, vsdf_buf, vmat_buf]:
		_rd.free_rid(rid)
	return out

func compare(ops: PackedByteArray, op_count: int, label: String) -> void:
	var vol := ball_volume()
	# The CPU side reads the same bytes through VoxelWorld's own ve::VolumeSet.
	_world.hooks().debug_store_volume(0, vol[0], vol[1], VDIM)
	var pts := sample_points()
	var gpu := run_gpu(pts, ops, op_count, vol)
	assert_int(gpu.size()).is_equal(pts.size() * 4)

	var worst := 0.0
	var within_one := 0
	var mat_mismatch := 0
	var mat_compared := 0
	for i in range(pts.size()):
		var cpu: Vector2 = _world.hooks().debug_eval_field(pts[i], ops, op_count)
		var diff: float = absf(gpu[i * 4] - cpu.x) / SDF_STEP
		worst = maxf(worst, diff)
		if diff <= 1.0:
			within_one += 1
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

func test_a_box_subtract_matches() -> void:
	# Cells 5..8 on x, 60..62 on y, 5..7 on z -> world [4.0, 7.2) x [48.0, 50.4) x [4.0, 6.4).
	compare(box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7)), 1, "box")

func test_a_chain_of_box_subtracts_matches() -> void:
	# What an island carve actually looks like: several boxes tiling one component.
	var ops := box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7))
	ops.append_array(box_op(Vector3i(9, 60, 5), Vector3i(9, 61, 7)))
	ops.append_array(box_op(Vector3i(5, 63, 5), Vector3i(6, 63, 6)))
	compare(ops, 3, "box chain")

func test_a_volume_add_matches() -> void:
	compare(volume_op(0), 1, "volume")

func test_a_carve_then_paste_chain_matches() -> void:
	# The full island lifecycle in one op list: the terrain is carved, and rubble is pasted
	# back somewhere else. Order matters on both sides.
	var ops := make_op(OP_SUBTRACT, 0, Vector3(8.0, 51.2, 8.0), 4.0)
	ops.append_array(box_op(Vector3i(5, 60, 5), Vector3i(8, 62, 7)))
	ops.append_array(volume_op(0))
	compare(ops, 3, "carve+paste")

func test_a_volume_op_naming_an_all_air_slot_agrees() -> void:
	# Fail-soft (spec section 8): an all-air sentinel stored in a pinned slot is the intended
	# mechanism for a slot that has not received real data. The production invariant is that
	# an op may only name a pinned slot, and pinning requires a stored, non-empty volume, so
	# the manager never lets an op reach the log without its slot stored and pinned. This
	# test pins that contract by giving both sides an all-air volume and requiring they
	# agree. It does NOT exercise the CPU empty-slot skip path -- that path is unreachable
	# through the log (test_volume_ops.cpp closes the reserve-without-store gap).
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	sdf.fill(255) # encode_sdf(+0.64): solidly outside anything
	mat.fill(0)
	_world.hooks().debug_store_volume(0, sdf, mat, VDIM)
	var pts := sample_points()
	var gpu := run_gpu(pts, volume_op(0), 1, [sdf, mat])
	for i in range(pts.size()):
		var cpu: Vector2 = _world.hooks().debug_eval_field(pts[i], volume_op(0), 1)
		assert_float(absf(gpu[i * 4] - cpu.x) / SDF_STEP).is_less(MAX_STEPS)
