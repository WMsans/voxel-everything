extends GdUnitTestSuite

var _worlds: Array = []

const OP_BOX_SUBTRACT := 3
const OP_VOLUME_ADD := 4

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

# ve::EditOp is 32 raw bytes and debug_brick_diff takes them as a PackedByteArray, exactly
# as test_brick_diff.gd already packs them: type u32, material u32, pos[3] f32, radius f32,
# aux[2] u32.
func op_bytes(type: int, material: int, c: Vector3, radius: float) -> PackedByteArray:
	return make_op(type, material, c, radius)

func make_op(type: int, material: int, c: Vector3, radius: float,
		aux0: int = 0, aux1: int = 0) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(c.x)
	b.put_float(c.y)
	b.put_float(c.z)
	b.put_float(radius)
	b.put_u32(aux0)
	b.put_u32(aux1)
	return b.data_array

func pack_extent3(nx: int, ny: int, nz: int) -> int:
	return nx | (ny << 10) | (nz << 20)

func generate_region(w: VoxelWorld, ops: PackedByteArray, op_count: int) -> void:
	var region := Vector3i(0, 2, 0)
	w.hooks().debug_upload_region_ops(0, ops, op_count)
	w.hooks().debug_mark_region(region, 0, region * 32, region * 32 + Vector3i(31, 31, 31), op_count, true)
	w.hooks().debug_generate_pending()

func run_filtered_gpu(pts: PackedVector3Array, ops: PackedByteArray, op_count: int,
		volume: Array) -> PackedFloat32Array:
	var code: String = _worlds[0].hooks().debug_load_shader("res://shaders/field_filter_probe.comp.glsl")
	assert_str(code).is_not_empty()
	code = code.replace("#[compute]\n", "")
	var rd := RenderingServer.create_local_rendering_device()
	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = code
	var spirv := rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()
	var pipeline := rd.compute_pipeline_create(shader)

	var op_bytes := ops.duplicate()
	if op_bytes.size() < 32:
		op_bytes.resize(32)
	var op_buf := rd.storage_buffer_create(op_bytes.size(), op_bytes)
	var point_bytes := PackedFloat32Array()
	for p in pts:
		point_bytes.append_array(PackedFloat32Array([p.x, p.y, p.z, 0.0]))
	var point_buf := rd.storage_buffer_create(point_bytes.size() * 4, point_bytes.to_byte_array())
	var out_buf := rd.storage_buffer_create(pts.size() * 16)
	var sdf_buf := rd.storage_buffer_create(volume[0].size(), volume[0])
	var mat_buf := rd.storage_buffer_create(volume[1].size(), volume[1])
	var uniforms := []
	for pair in [[0, op_buf], [1, point_buf], [2, out_buf], [3, sdf_buf], [4, mat_buf]]:
		var u := RDUniform.new()
		u.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
		u.binding = pair[0]
		u.add_id(pair[1])
		uniforms.append(u)
	var uset := rd.uniform_set_create(uniforms, shader, 0)
	var push := PackedInt32Array([pts.size(), op_count, 0, 0]).to_byte_array()
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uset, 0)
	rd.compute_list_set_push_constant(list, push, push.size())
	rd.compute_list_dispatch(list, (pts.size() + 63) / 64, 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()
	var result := rd.buffer_get_data(out_buf).to_float32_array()
	for rid in [uset, pipeline, shader, op_buf, point_buf, out_buf, sdf_buf, mat_buf]:
		rd.free_rid(rid)
	rd.free()
	return result

func compare_filtered_to_unfiltered(pts: PackedVector3Array, ops: PackedByteArray,
		op_count: int, volume: Array, label: String) -> void:
	if _worlds.is_empty():
		make_world()
	_worlds[0].hooks().debug_store_volume(0, volume[0], volume[1], 2)
	var gpu := run_filtered_gpu(pts, ops, op_count, volume)
	assert_int(gpu.size()).is_equal(pts.size() * 4)
	for i in range(pts.size()):
		var oracle: Vector2 = _worlds[0].hooks().debug_eval_field(pts[i], ops, op_count)
		# Consumers store an R8 lattice, so the independent oracle compares the representable
		# field rather than an out-of-band CSG value that both sides clamp identically.
		var expected_sdf := clampf(oracle.x, -0.64, 0.64)
		var got_sdf := clampf(gpu[i * 4], -0.64, 0.64)
		var sdf_diff := absf(got_sdf - expected_sdf)
		assert_float(sdf_diff).override_failure_message(
			"%s point %d differs from unfiltered oracle by %f m" % [label, i, sdf_diff]
			).is_less(2.0 / 255.0)
		if absf(expected_sdf) < 0.63:
			assert_int(int(gpu[i * 4 + 1])).override_failure_message(
			"%s point %d material differs" % [label, i]).is_equal(int(oracle.y))

func test_filtered_box_matches_the_unfiltered_oracle() -> void:
	var ops := make_op(OP_BOX_SUBTRACT, 0, Vector3(1.4, 40.0, 0.0), 0.0,
		pack_extent3(1, 1, 1), 0)
	var pts := PackedVector3Array([Vector3(0.8, 40.0, 0.0), Vector3(5.0, 40.0, 0.0)])
	compare_filtered_to_unfiltered(pts, ops, 1, [PackedByteArray([255, 255, 255, 255, 255, 255, 255, 255]),
		PackedByteArray([0, 0, 0, 0, 0, 0, 0, 0])],
		"box")

func test_filtered_volume_matches_the_unfiltered_oracle() -> void:
	var sdf := PackedByteArray([255, 255, 255, 255, 255, 255, 255, 255])
	var mat := PackedByteArray([0, 0, 0, 0, 0, 0, 0, 0])
	var ops := make_op(OP_VOLUME_ADD, 0, Vector3(1.4, 80.0, 0.0), 0.05, 0, 2)
	var pts := PackedVector3Array([Vector3(0.8, 80.0, 0.0), Vector3(5.0, 80.0, 0.0)])
	compare_filtered_to_unfiltered(pts, ops, 1, [sdf, mat], "volume")

# The differential test that matters: with a long op list where most ops are far away, the
# GPU brick must still equal the CPU brick. debug_brick_diff is M2's existing hook -- it
# generates one brick on the GPU and compares every voxel against ve::eval_brick.
func test_generated_brick_matches_the_cpu_with_a_long_op_list(timeout := 30000) -> void:
	var w := make_world()
	var ops := PackedByteArray()
	# 200 ops: four of them land on the brick under test, the rest are scattered.
	var target := Vector3(8.4, 54.4, 8.4)
	for i in range(200):
		var c: Vector3 = target if i % 50 == 0 else target + Vector3(
			cos(float(i)) * (5.0 + float(i) * 0.1), sin(float(i)) * 2.0,
			sin(float(i) * 1.3) * (5.0 + float(i) * 0.1))
		ops.append_array(op_bytes(1, 4, c, 0.5))
	generate_region(w, ops, 200)
	var d: Dictionary = w.hooks().debug_brick_diff(Vector3i(10, 68, 10), 0, ops, 200)
	assert_int(int(d["sdf_diff_over_one"])).is_equal(0)
	assert_int(int(d["mat_near_mismatch"])).is_equal(0)

# Order is the other half of correctness: subtract-then-add is not add-then-subtract, and a
# filter that reorders would pass a sample-count test while producing a different world.
func test_filter_preserves_op_order(timeout := 30000) -> void:
	var w := make_world()
	var c := Vector3(8.4, 54.4, 8.4)
	var ops := PackedByteArray()
	ops.append_array(op_bytes(0, 0, c, 2.0))       # subtract
	ops.append_array(op_bytes(1, 4, c, 1.0))       # add back inside the hole
	for i in range(50):                             # far-away filler
		ops.append_array(op_bytes(0, 0, Vector3(c.x + 40.0 + float(i), c.y, c.z), 1.0))
	generate_region(w, ops, 52)
	var d: Dictionary = w.hooks().debug_brick_diff(Vector3i(10, 68, 10), 0, ops, 52)
	assert_int(int(d["sdf_diff_over_one"])).is_equal(0)
