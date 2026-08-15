extends GdUnitTestSuite

# Regression test for the Task 9 review finding: the edit tool's explosion kick was being
# overwritten by the player's walking/gravity velocity assignment before move_and_slide().
# The player now stores a pending _impulse and applies it immediately before move_and_slide().

func test_walk_mode_kick_is_applied_once_before_move_and_slide() -> void:
	var player := CharacterBody3D.new()
	player.set_script(load("res://demo/player.gd"))
	var cam := Camera3D.new()
	cam.name = "Camera3D"
	player.add_child(cam)
	player.flying = false
	add_child(player)
	await get_tree().physics_frame

	player._impulse = Vector3(2.0, 1.0, -3.0)
	player._physics_process(1.0 / 60.0)

	var gravity_delta: float = 24.0 / 60.0
	assert_float(player.velocity.x).is_equal_approx(2.0, 0.001)
	assert_float(player.velocity.y).is_equal_approx(1.0 - gravity_delta, 0.001)
	assert_float(player.velocity.z).is_equal_approx(-3.0, 0.001)
	assert_vector(player._impulse).is_equal_approx(Vector3.ZERO, Vector3(0.001, 0.001, 0.001))
	player.free()
