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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# THE differential test of this milestone. shade.glslh's cel_shade and ve::cel_shade are two
# spellings of one function; if they drift, the near field and the object materials part
# company and the seam this whole design exists to hide becomes visible again.
func test_the_gpu_cel_ramp_matches_the_cpu_one() -> void:
	var w := make_world()
	var cases := [
		# albedo,                       ambient,                  ndl, ndv, ndh, shadow, ao, gloss
		[Color(0.8, 0.2, 0.1), Color(0.0, 0.0, 0.0), 1.0, 1.0, 0.0, 1.0, 1.0, 0.0],
		[Color(0.36, 0.55, 0.22), Color(0.16, 0.19, 0.26), 0.5, 0.8, 0.3, 1.0, 1.0, 0.0],
		[Color(0.45, 0.42, 0.40), Color(0.16, 0.19, 0.26), 0.05, 0.4, 0.9, 1.0, 1.0, 0.9],
		[Color(0.45, 0.42, 0.40), Color(0.16, 0.19, 0.26), 0.9, 0.1, 0.1, 0.0, 1.0, 0.0],
		[Color(0.5, 0.5, 0.5), Color(0.2, 0.2, 0.2), 0.33, 0.5, 0.75, 0.5, 0.4, 0.6],
		[Color(0.02, 0.02, 0.9), Color(0.0, 0.0, 0.0), 0.66, 1.0, 0.0, 1.0, 1.0, 0.0],
	]
	for c in cases:
		var d: Dictionary = w.hooks().debug_cel_diff(c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7])
		assert_float(d["max_delta"]).override_failure_message(
			"gpu %s vs cpu %s for %s" % [d["gpu"], d["cpu"], c]).is_less(0.004)

# The band edges are the whole look. If the GPU search rounds differently from the CPU one,
# the terracing lands a pixel off and the outlines stop lining up with the bands.
func test_the_band_edges_land_in_the_same_place_on_both_sides() -> void:
	var w := make_world()
	for edge in [0.08, 0.32, 0.66]:
		for delta in [-0.01, 0.01]:
			var d: Dictionary = w.hooks().debug_cel_diff(Color(0.6, 0.6, 0.6), Color(0, 0, 0),
				edge + delta, 1.0, 0.0, 1.0, 1.0, 0.0)
			assert_float(d["max_delta"]).override_failure_message(
				"band edge %f%+f: gpu %s cpu %s" % [edge, delta, d["gpu"], d["cpu"]]
				).is_less(0.004)

# Reconstructing world position from the depth attachment is where a sign error hides: it
# looks plausible everywhere and is wrong everywhere. Pin it against the position the
# raymarcher actually hit.
func test_the_deferred_pass_reconstructs_the_world_position_it_was_given() -> void:
	var w := make_world()
	var pos := Vector3(20.0, 75.0, 20.0)
	var fwd := Vector3(0, -1, 0)
	var truth := w.hooks().debug_raymarch_gbuffer(pos, fwd)
	assert_bool(truth["hit"]).is_true()
	# probe_mode 2 writes the reconstructed world position into the lit target instead of a
	# colour; the probe reports the centre pixel.
	var d: Dictionary = w.hooks().debug_deferred_probe(pos, fwd, 64, 64, 2)
	var got: Vector3 = d["center"]
	var want: Vector3 = truth["position"]
	assert_float(got.distance_to(want)).override_failure_message(
		"reconstructed %s, raymarched %s" % [got, want]).is_less(0.25)

func test_sky_pixels_pass_through_the_deferred_pass_unlit() -> void:
	var w := make_world()
	# Straight up: every pixel is sky, material 0.
	var d: Dictionary = w.hooks().debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0), 64, 64, 0)
	var c: Color = d["center"]
	# sky_color() is blue-dominant looking up. Cel-shading it would band it into flat plates.
	assert_float(c.b).is_greater(c.r)
	assert_int(d["distinct_rows"]).override_failure_message(
		"the sky was quantised into cel bands").is_greater(8)

func test_the_lit_image_is_darker_where_the_sun_ray_says_it_is() -> void:
	var w := make_world()
	var d_lit: Dictionary = w.hooks().debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var d_flat: Dictionary = w.hooks().debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	# Removing the shadow term can only brighten the image, never darken it.
	assert_float(d_flat["mean_luma"]).is_greater_equal(float(d_lit["mean_luma"]) - 0.001)
