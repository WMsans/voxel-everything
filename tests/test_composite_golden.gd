extends GdUnitTestSuite

# Characterization, not specification. These numbers are whatever the composite's
# overlay path produced on 2026-09-15; they exist so that "the composite did not
# change" is a measurement rather than an assertion. If an intentional redesign moves
# them, re-record them in the same commit that causes the move and say so in the
# message.
#
# Why this probe bites where test_raymarch_gbuffer.gd does not: that suite only looks
# at ordinary lit hits, where there is nothing to overlay, so the overlay weight is
# zero and whichever texture sits at binding 0 is multiplied away. A miss carries the
# sky in the overlay at full weight, so a sky view through the real composite sees the
# swap: binding the surface texture as the overlay turns the sky black
# (center_albedo (0.251, 0.451, 0.851) -> (0, 0, 0)). The ground control below the
# band proves the break is specific to the overlay: its albedo does not move.
#
# Same world construction as tests/test_raymarch_gbuffer.gd: a local rendering device,
# physics off, streamed until the chunk queue goes quiet.

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
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# Three repeated runs on 2026-09-15 were bit-identical (spread 0), so this tolerance is
# a fixed floor, not twice a measured spread: the break moves every sky channel by more
# than 60x TOL. If this ever goes flaky, widen TOL before re-recording.
const SKY_ALBEDO := Color(0.251, 0.451, 0.851)
const GND_ALBEDO := Color(0.2941, 0.2745, 0.102)
const TOL := 0.004

func test_sky_through_the_composite_matches_the_recorded_golden(timeout := 120000) -> void:
	var w := make_world()
	var sky: Dictionary = w.hooks().debug_near_field_detail(
		Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0), 64, 64, 1.0)
	assert_bool(sky["ran"]).override_failure_message(
		"composite did not run").is_true()
	var c: Color = sky["center_albedo"]
	assert_float(c.r).override_failure_message(
		"sky red moved: golden %f, got %f" % [SKY_ALBEDO.r, c.r]
	).is_equal_approx(SKY_ALBEDO.r, TOL)
	assert_float(c.g).override_failure_message(
		"sky green moved: golden %f, got %f" % [SKY_ALBEDO.g, c.g]
	).is_equal_approx(SKY_ALBEDO.g, TOL)
	assert_float(c.b).override_failure_message(
		"sky blue moved: golden %f, got %f" % [SKY_ALBEDO.b, c.b]
	).is_equal_approx(SKY_ALBEDO.b, TOL)

func test_lit_ground_through_the_composite_matches_the_recorded_golden(timeout := 120000) -> void:
	var w := make_world()
	var gnd: Dictionary = w.hooks().debug_near_field_detail(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 1.0)
	assert_bool(gnd["ran"]).override_failure_message(
		"composite did not run").is_true()
	var c: Color = gnd["center_albedo"]
	assert_float(c.r).override_failure_message(
		"ground red moved: golden %f, got %f" % [GND_ALBEDO.r, c.r]
	).is_equal_approx(GND_ALBEDO.r, TOL)
	assert_float(c.g).override_failure_message(
		"ground green moved: golden %f, got %f" % [GND_ALBEDO.g, c.g]
	).is_equal_approx(GND_ALBEDO.g, TOL)
	assert_float(c.b).override_failure_message(
		"ground blue moved: golden %f, got %f" % [GND_ALBEDO.b, c.b]
	).is_equal_approx(GND_ALBEDO.b, TOL)
