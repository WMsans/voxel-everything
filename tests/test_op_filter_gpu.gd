extends GdUnitTestSuite

var _worlds: Array = []

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
	assert_bool(w.debug_init_atlas()).is_true()
	return w

# ve::EditOp is 32 raw bytes and debug_brick_diff takes them as a PackedByteArray, exactly
# as test_brick_diff.gd already packs them: type u32, material u32, pos[3] f32, radius f32,
# aux[2] u32.
func op_bytes(type: int, material: int, c: Vector3, radius: float) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type)
	b.put_u32(material)
	b.put_float(c.x)
	b.put_float(c.y)
	b.put_float(c.z)
	b.put_float(radius)
	b.put_u32(0)
	b.put_u32(0)
	return b.data_array

func generate_region(w: VoxelWorld, ops: PackedByteArray, op_count: int) -> void:
	var region := Vector3i(0, 2, 0)
	w.debug_upload_region_ops(0, ops, op_count)
	w.debug_mark_region(region, 0, region * 32, region * 32 + Vector3i(31, 31, 31), op_count, true)
	w.debug_generate_pending()

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
	var d: Dictionary = w.debug_brick_diff(Vector3i(10, 68, 10), 0, ops, 200)
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
	var d: Dictionary = w.debug_brick_diff(Vector3i(10, 68, 10), 0, ops, 52)
	assert_int(int(d["sdf_diff_over_one"])).is_equal(0)
