extends GdUnitTestSuite
# The gameplay raycast (docs/superpowers/specs/2026-09-16-world-field-query-design.md §5):
# VoxelWorld.raycast is what demo/edit_tool.gd and demo/hud.gd aim with, and debug_raycast is
# a forward to it.

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
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

func test_raycast_reports_what_debug_raycast_reports() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	for origin in [Vector3(24.4, 70.0, 24.4), Vector3(6.4, 70.0, 20.0), Vector3(10.0, 80.0, 10.0)]:
		var a: Dictionary = w.raycast(origin, Vector3(0.2, -1.0, 0.1))
		var b: Dictionary = w.hooks().debug_raycast(origin, Vector3(0.2, -1.0, 0.1))
		assert_dict(a).is_equal(b)

func test_a_hit_carries_position_normal_distance_and_material() -> void:
	var w := make_world()
	var h: Dictionary = w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.DOWN)
	assert_bool(h["hit"]).is_true()
	assert_float(float(h["pos"].y)).is_between(41.0, 62.0)
	assert_float(float(h["normal"].y)).is_greater(0.0)
	assert_float(float(h["distance"])).is_equal_approx(80.0 - float(h["pos"].y), 0.01)
	assert_int(int(h["material"])).is_greater(0)

func test_max_distance_bounds_the_ray() -> void:
	var w := make_world()
	assert_bool(w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.DOWN, 10.0)["hit"]).is_false()
	assert_bool(w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.DOWN)["hit"]).is_true()
	assert_bool(w.raycast(Vector3(12.8, 80.0, 12.8), Vector3.UP)["hit"]).is_false()
