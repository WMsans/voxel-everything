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

# Debug probes use a local RenderingDevice, so this is the runtime path that must publish the
# cached scene SunState rather than leaving the orchestrator's white kSunDir seed in place.
func test_authored_sun_drives_direct_direction_and_colour(timeout := 60000) -> void:
	var w := make_world()
	w.set_effect_enabled("sun_shadow_map", false)
	w.set_effect_enabled("raymarched_sun_shadow", false)
	w.set_effect_enabled("ssgi", false)
	w.set_effect_enabled("ssao", false)
	var light := DirectionalLight3D.new()
	light.rotation = Vector3(-0.8, 0.0, 0.0)
	light.light_color = Color(1.0, 0.0, 0.0)
	light.light_energy = 1.0
	add_child(light)
	w.sun_light_path = w.get_path_to(light)
	await get_tree().process_frame
	var red: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	assert_bool(red.has("center")).is_true()
	var red_center: Color = red["center"]

	light.light_color = Color(0.0, 0.0, 1.0)
	await get_tree().process_frame
	var blue: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	assert_bool(blue.has("center")).is_true()
	var blue_center: Color = blue["center"]
	assert_float(red_center.r - blue_center.r).override_failure_message(
			"authored sun colour did not reach direct lighting: red=%s blue=%s" % [red_center, blue_center]
			).is_greater(0.02)
	assert_float(blue_center.b - red_center.b).override_failure_message(
			"authored sun colour did not reach direct lighting: red=%s blue=%s" % [red_center, blue_center]
			).is_greater(0.02)

	light.light_color = Color(1.0, 1.0, 1.0)
	light.rotation = Vector3(-0.2, 0.0, 0.0)
	await get_tree().process_frame
	var high: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	light.rotation = Vector3(-1.2, 0.0, 0.0)
	await get_tree().process_frame
	var low: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	assert_float(absf(float(high["mean_luma"]) - float(low["mean_luma"]))).override_failure_message(
			"authored sun direction did not change direct lighting: high=%s low=%s" %
			[high["mean_luma"], low["mean_luma"]]).is_greater(0.01)
	light.queue_free()


# THE regression this fix exists for. cel_shade ends with `+ vec3(rim)` -- an UNMODULATED
# WHITE term driven by pow(1 - ndv, 3), and the only unmodulated-white term in the ramp. A
# rim light is a stylization of SILHOUETTE, and that driver is only a silhouette proxy on
# closed, curved geometry. On open ground the normal is up and the view is level, so ndv
# falls to 0 with DISTANCE and never recovers: the rim climbed to its full 0.35 across the
# whole far field and painted it white. Decomposed in linear space, the far field measured
# +0.153/+0.160/+0.158 over the near field -- equal in all three channels, which is what
# identifies it as additive white rather than fog or a lighting scale.
#
# Measured DIFFERENTIALLY, against the same frame with the gate forced open, because that is
# the pre-fix shader exactly. Holding the geometry fixed is what makes the numbers mean
# something: terrain shading varies far more between two distance bands than the rim does, so
# an absolute near-vs-far comparison would measure the dunes rather than the bug.
#
# The pitch is steep enough that the farthest quartile is still open ground. At a shallow
# pitch the farthest surface pixels ARE the skyline, which is the one place the gate is
# supposed to fire, and the differential collapses for the right reason and pins nothing.
func test_the_rim_gate_takes_more_white_off_far_ground_than_near_ground() -> void:
	var w := make_world()
	var cam := Vector3(20.0, 70.0, 20.0)
	var fwd := Vector3(0.0, -1.1, -1.0)
	var gated: Dictionary = w.hooks().debug_deferred_probe(cam, fwd, 192, 144, 0)
	assert_bool(gated.has("far_luma")).override_failure_message(
			"probe reported no surface bands: %s" % gated).is_true()

	var src := FileAccess.get_file_as_string("res://shaders/deferred.comp.glsl")
	var ungated := src.replace("silhouette_gate(px, size)", "1.0")
	assert_str(ungated).override_failure_message(
			"the gate call site moved -- this test no longer reproduces the pre-fix shader"
			).is_not_equal(src)
	w.hooks().debug_set_shader_override("deferred.comp.glsl", ungated)
	w.hooks().debug_request_shader_reload()
	w.hooks().debug_pump_shader_reload()
	assert_bool(w.hooks().debug_shader_reload_stats()["last_ok"]).override_failure_message(
			"ungated deferred shader did not compile: %s" % w.hooks().debug_shader_reload_stats()
			).is_true()
	var ungated_probe: Dictionary = w.hooks().debug_deferred_probe(cam, fwd, 192, 144, 0)
	w.hooks().debug_clear_shader_source_overrides()
	w.hooks().debug_request_shader_reload()
	w.hooks().debug_pump_shader_reload()

	# Non-vacuity: the frame has to span a real distance range, or "far" means nothing.
	assert_float(float(gated["far_dist"])).override_failure_message(
			"frame does not span a distance range: %s" % gated
			).is_greater(float(gated["near_dist"]) * 1.5)
	var far_gain := float(ungated_probe["far_luma"]) - float(gated["far_luma"])
	var near_gain := float(ungated_probe["near_luma"]) - float(gated["near_luma"])
	# Measured 0.0615 far against 0.0002 near: the ungated rim is essentially absent underfoot
	# and worth a sixteenth of the frame's luminance two dune-lengths out. That gap IS the
	# distance ramp the eye reads as "the far LoD is whiter", and the gate has to remove it
	# rather than merely dim the whole frame.
	assert_float(far_gain).override_failure_message(
			"gate took no white off far ground: gated=%s ungated=%s" %
			[gated["far_luma"], ungated_probe["far_luma"]]).is_greater(0.03)
	assert_float(far_gain - near_gain).override_failure_message(
			"gate did not flatten the distance ramp: far_gain=%f near_gain=%f" %
			[far_gain, near_gain]).is_greater(0.02)

# The gate's own reading, on the shipping shader. Ground is continuous however grazing it
# gets, so nothing on it is a silhouette and the rim must be off across the whole frame --
# this is the assertion that stops the ramp coming back.
func test_the_rim_gate_is_off_over_open_ground() -> void:
	var w := make_world()
	var probe: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 70.0, 20.0), Vector3(0.0, -1.0, 0.0), 192, 144, 5)
	# Non-vacuity: looking straight down must actually see ground, or a frame of sky would
	# read 0 and pass for the wrong reason.
	assert_int(probe["surface_pixels"]).override_failure_message(
			"no ground under the camera: %s" % probe).is_greater(192 * 144 / 2)
	assert_float(float(probe["mean_luma"])).override_failure_message(
			"rim gate fires over open ground: mean=%s" % probe["mean_luma"]).is_less(0.02)

# ... and it must still fire where the surface genuinely ENDS, or the fix has simply deleted
# the stylization. The same camera tilted up until the horizon is in frame has a skyline, and
# a skyline is the silhouette the rim was written for.
func test_the_rim_gate_still_fires_on_a_skyline() -> void:
	var w := make_world()
	var down: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 70.0, 20.0), Vector3(0.0, -1.0, 0.0), 192, 144, 5)
	var horizon: Dictionary = w.hooks().debug_deferred_probe(
			Vector3(20.0, 70.0, 20.0), Vector3(0.0, -0.06, -1.0), 192, 144, 5)
	# Compared against the ground-only frame rather than against an absolute number: what the
	# gate answers to is background in the neighbourhood, and the only difference between these
	# two frames is that one has a horizon in it.
	assert_float(float(horizon["mean_luma"])).override_failure_message(
			"rim gate never fires, so the stylization is gone entirely: down=%s horizon=%s" %
			[down["mean_luma"], horizon["mean_luma"]]
			).is_greater(float(down["mean_luma"]) + 0.01)
