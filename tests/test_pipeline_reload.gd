extends GdUnitTestSuite

# A broken stage must never kill a running world: preflight compiles every consumer against
# the new field BEFORE anything is torn down, so last-known-good pipelines survive and the
# error is reported instead of thrown.
#
# Shape mirrors tests/test_shader_reload.gd (local device, synchronous pump hook) rather
# than frame-pumping: a local-device world has no render-thread callback to pick the
# latch up, so debug_pump_shader_reload does exactly what that callback would do,
# deterministically.

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
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	return w

func test_a_broken_field_override_leaves_the_world_alive(timeout := 60000) -> void:
	var w := make_world()
	var before: PackedFloat32Array = w.hooks().debug_generator_fingerprint()
	assert_int(before.size()).is_greater(0)

	w.hooks().debug_set_shader_override("field.glslh",
		"this is not valid GLSL at all\n")
	w.hooks().debug_request_shader_reload()
	w.hooks().debug_pump_shader_reload()
	assert_bool(w.is_initialized()).is_true()

	var stats: Dictionary = w.hooks().debug_shader_reload_stats()
	assert_bool(stats["last_ok"]).is_false()
	assert_str(str(stats["last_error"])).is_not_empty()

	# The world still renders and the CPU field is untouched.
	var after: PackedFloat32Array = w.hooks().debug_generator_fingerprint()
	assert_int(after.size()).is_equal(before.size())
	for i in range(before.size()):
		assert_float(after[i]).is_equal_approx(before[i], 1e-5)

	w.hooks().debug_clear_shader_source_overrides()
