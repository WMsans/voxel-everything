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
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# HBAO runs one pixel per G-buffer pixel: no half-res target, no upsample blur step to
# fight the cel look. If this ever changes, it is a deliberate redesign, not an accident.
func test_the_output_is_full_resolution() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_int(d["width"]).is_equal(128)
	assert_int(d["height"]).is_equal(128)

func test_occlusion_is_a_bounded_fraction() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_bool(d["ran"]).is_true()
	# AO multiplies the ambient term, so it must live in [0, 1]: never brighten, never sign-flip.
	assert_float(d["min_ao"]).is_greater_equal(0.0)
	assert_float(d["max_ao"]).is_less_equal(1.001)

# Looking down into the crater at (30, ~49, 30): walls see each other, so HBAO must find
# real geometric occlusion somewhere in the frame — not just pass the depth buffer through.
func test_crater_geometry_produces_real_occlusion() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(d["min_ao"]).override_failure_message(
		"a frame full of open geometry produced no occlusion at all").is_less(0.95)

# ...while the sky (material 0, no surface behind it) must stay untouched: darkened skies
# are the classic SSAO bug and the fastest way to ruin a horizon.
func test_open_sky_stays_unoccluded() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.35, -0.2, 0.35).normalized(), 128, 128)
	assert_float(d["max_ao"]).override_failure_message(
		"sky pixels were darkened by screen-space occlusion").is_greater(0.99)

func test_turning_ssao_off_costs_no_dispatch() -> void:
	var w := make_world()
	w.set_effect_enabled("ssao", false)
	var d: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_bool(d["ran"]).is_false()

# The whole point: with the same frame rendered twice, AO applied to the ambient term must
# darken the lit image (sun/spec/rim are identical between the runs). But ambient-only means
# the darkening is bounded — a bug that multiplied the whole lighting would fail the floor.
func test_ambient_darkens_but_stays_bounded() -> void:
	var w := make_world()
	var on: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_bool(on["ran"]).is_true()
	w.set_effect_enabled("ssao", false)
	var off: Dictionary = w.hooks().debug_ssao_probe(Vector3(30.0, 70.0, 30.0),
		Vector3(0.2, -1.0, 0.2).normalized(), 128, 128)
	assert_float(on["lit_luma"]).override_failure_message(
		"applying SSAO did not change the lit image at all").is_less(off["lit_luma"])
	assert_float(on["lit_luma"]).override_failure_message(
		"SSAO more than halved total luma — it must touch only the ambient term").is_greater(
		float(off["lit_luma"]) * 0.5)
