extends GdUnitTestSuite
# GroundSpawn against a stub world, so no GPU is involved: what matters is when it moves the
# player and that it always leaves.

const GroundSpawn := preload("res://demo/scripts/ground_spawn.gd")

var _holder: Node

func after_test() -> void:
	if is_instance_valid(_holder):
		_holder.free()

func _rig(answer: Variant, timeout_s := 10.0) -> Array:
	var stub := GDScript.new()
	stub.source_code = "extends Node\nvar answer = null\nfunc ground_at(_x, _z):\n\treturn answer\n"
	stub.reload()
	_holder = Node.new()
	var world := Node.new()
	world.name = "VoxelWorld"
	world.set_script(stub)
	world.set("answer", answer)
	_holder.add_child(world)
	var player := Node3D.new()
	player.name = "Player"
	player.position = Vector3(8, 62, 8)
	_holder.add_child(player)
	var spawn := GroundSpawn.new()
	spawn.timeout_s = timeout_s
	_holder.add_child(spawn)
	add_child(_holder)
	return [world, player, spawn]

func _frames(n: int) -> void:
	for i in range(n):
		await get_tree().process_frame

func test_the_player_lands_two_metres_above_the_ground() -> void:
	var r := _rig(Vector3(8, 120, 8))
	await _frames(3)
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 122, 8))
	assert_bool(is_instance_valid(r[2])).is_false()

func test_it_waits_while_the_world_is_not_ready() -> void:
	var r := _rig(null)
	await _frames(3)
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 62, 8))
	assert_bool(is_instance_valid(r[2])).is_true()
	r[0].set("answer", Vector3(8, 40, 8))
	await _frames(3)
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 42, 8))

func test_a_world_that_never_initialises_is_given_up_on() -> void:
	var r := _rig(null, 0.05)
	await _frames(30)
	assert_bool(is_instance_valid(r[2])).is_false()
	assert_vector((r[1] as Node3D).position).is_equal(Vector3(8, 62, 8))
