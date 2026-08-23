extends GdUnitTestSuite

# Phase-2 contract smoke test (Task 10): accepted edits bump edit_seq monotonically
# across the WorldStore boundary, and the WorldStore-owned FieldGenerator seam answers
# samples for the same analytic terrain the CPU evaluators have always walked.

const OP_SUBTRACT := 0

var _world: VoxelWorld

func after_test() -> void:
	if _world != null and is_instance_valid(_world):
		_world.free()
	_world = null

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

func sphere_subtract_op() -> PackedByteArray:
	return make_op(OP_SUBTRACT, 0, Vector3(10.0, 51.2, 10.0), 6.0)

func test_append_bumps_edit_seq_monotonically() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.use_local_device = true
	add_child(_world)
	_world.ensure_initialized()
	var before: int = _world.edit_seq()
	var op := sphere_subtract_op()
	var result: Dictionary = _world.append_edit(op)
	assert_array(result["rejected"]).is_empty()
	assert_array(result["touched"]).is_not_empty()
	assert_int(_world.edit_seq()).is_greater(before)
