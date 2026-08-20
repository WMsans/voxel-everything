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
	w.ensure_initialized()
	return w

# A reload must leave a working world behind, not merely survive: the same probe answers the
# same way before and after, because the CPU cores that describe the world never went away.
func test_reload_keeps_the_world(timeout := 60000) -> void:
	var w := make_world()
	w.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 1.5)
	var before: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	w.request_shader_reload()
	w.debug_pump_shader_reload() # what the render callback does
	assert_bool(w.is_initialized()).is_true()
	assert_int(int(w.debug_shader_reload_stats()["reloads"])).is_equal(1)
	assert_bool(w.debug_shader_reload_stats()["last_ok"]).is_true()
	var after: Dictionary = w.debug_raycast(Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0))
	assert_bool(after["hit"]).is_equal(before["hit"])

# A shader that will not compile must leave the previous pipelines running (spec §8's
# fail-soft rule), not a black screen.
func test_a_broken_reload_keeps_the_old_pipelines(timeout := 60000) -> void:
	var w := make_world()
	w.debug_set_shader_override("raymarch.comp.glsl", "#version 460\nthis is not glsl\n")
	w.request_shader_reload()
	w.debug_pump_shader_reload()
	assert_bool(w.debug_shader_reload_stats()["last_ok"]).is_false()
	assert_bool(w.is_initialized()).is_true()
	var d: Dictionary = w.debug_raymarch_cost_probe(Vector3(24.0, 70.0, 24.0), Vector3(0, -1, 0))
	assert_bool(d["hit"]).is_true()
