extends GdUnitTestSuite

# Emissive terrain as a LIGHT rather than as a bright pixel. test_material_glow.gd pins the
# crack's own shading; these pin what it does to everything else, which is the part that was
# missing: the glow value reached the lit buffer and stopped there.

var _worlds: Array = []
var _nodes: Array = []

const CENTRE := Vector3(30.0, 56.2, 30.0)
const EYE := Vector3(30.0, 70.0, 30.0)
var FWD := Vector3(0.2, -1.0, 0.2).normalized()

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()
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
	settle(w)
	return w

# Stream from the EDIT's centre until quiet: a paint dirties bricks and they must regenerate
# before any probe can see the new material. See the same note in test_material_glow.gd.
func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(CENTRE) == 0 else 0
		if quiet >= 6:
			break

func magma_id(w: VoxelWorld) -> int:
	for m in w.material_table():
		if float(m["glow"]) > 0.0:
			return int(m["id"])
	return -1

func paint_lava(w: VoxelWorld) -> void:
	w.hooks().debug_apply_sphere_paint(CENTRE, 10.0, magma_id(w))
	settle(w)

# THE test for this feature. SSGI is the only path by which one surface lights another, and
# before the emissive gather existed it carried essentially nothing from a glowing material:
# the lit history it bounced from is a temporal accumulator, and the bounce term averages
# over its taps, so one bright tap in eight read no brighter than one dull one.
func test_an_emissive_material_lights_the_surfaces_around_it() -> void:
	var w := make_world()
	var dark: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	paint_lava(w)
	var lit: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	assert_float(float(lit["max_channel"])).override_failure_message(
		"lava raised the gathered light from %f to only %f: emission is not being transported"
		% [float(dark["max_channel"]), float(lit["max_channel"])]
		).is_greater(float(dark["max_channel"]) * 10.0)

# The regression that made the original complaint ("I can barely see its light even with the
# camera pressed against the ground"). The screen-space tap offset used to be divided by
# distance-to-camera CLAMPED AT ONE METRE: closing in past that stopped shrinking the ring,
# the taps kept landing further than the radius away in world space, and the range test threw
# nearly all of them out. Approaching a light must not dim it.
func test_closing_in_on_the_light_does_not_dim_it() -> void:
	var w := make_world()
	paint_lava(w)
	var far: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	var near: Dictionary = w.hooks().debug_ssgi_probe(Vector3(30.0, 55.0, 30.0), FWD, 128, 128, 24)
	assert_float(float(near["mean_luma"])).override_failure_message(
		"stepping closer to the lava took the gathered light from %f down to %f"
		% [float(far["mean_luma"]), float(near["mean_luma"])]
		).is_greater(float(far["mean_luma"]))

# The reported bug: lit ground read as a lattice of dots instead of a wash. The gather rotates
# its spiral by bayer4(px), a rotation that never changes from frame to frame, so the temporal
# blend cannot average it out -- each pixel kept its own sampling luck, and with a few taps
# over a 16 m ring that luck is most of the signal. lattice_ratio is the mean deviation of a
# pixel from the box over one bayer period around it, as a fraction of the light.
func test_the_gathered_light_is_smooth_not_a_bayer_lattice() -> void:
	var w := make_world()
	paint_lava(w)
	var d: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	assert_float(float(d["mean_luma"])).override_failure_message(
		"the probe gathered no light; this test would pass vacuously").is_greater(0.01)
	print("ssgi lattice_ratio=", d["lattice_ratio"], " mean_luma=", d["mean_luma"])
	assert_float(float(d["lattice_ratio"])).override_failure_message(
		"gathered light deviates %f from its own 4x4 box: the bayer rotation is on screen"
		% float(d["lattice_ratio"])).is_less(0.1)

# Turning the effect off must take the emissive transport with it, or the knob is a lie and
# the cost cannot be recovered on a lower tier.
func test_disabling_ssgi_removes_the_emissive_transport() -> void:
	var w := make_world()
	paint_lava(w)
	w.set_effect_enabled("ssgi", false)
	var d: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	assert_bool(d["ran"]).is_false()

# The strength knob has to reach the shader. It lived as a literal in SsgiPass::render until
# this work, which is exactly the state ve::BeautySettings says no pass may be in.
func test_the_emissive_strength_knob_reaches_the_shader() -> void:
	var w := make_world()
	paint_lava(w)
	w.set_effect_value("emissive_gi_strength", 0.0)
	var off: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	w.set_effect_value("emissive_gi_strength", 24.0)
	var on: Dictionary = w.hooks().debug_ssgi_probe(EYE, FWD, 128, 128, 24)
	assert_float(float(on["max_channel"])).override_failure_message(
		"the emissive strength knob did not change the gathered light"
		).is_greater(float(off["max_channel"]) * 2.0)

# The bug that blew out an entire test frame: Godot's glow threshold is an absolute HDR level,
# so it has to sit ABOVE the sky or the sky blooms too. Emissive terrain is the only thing in
# this scene that reaches HDR, so the separation is real -- but only while it is measured.
func test_the_demo_bloom_threshold_clears_the_sky() -> void:
	var w := make_world()
	var sky: Dictionary = w.hooks().debug_deferred_probe(EYE, Vector3(0, 1, 0), 64, 64, 0)
	var c: Color = sky["center"]
	var peak := maxf(c.r, maxf(c.g, c.b))
	assert_float(peak).override_failure_message(
		"the sky probe read black; this test would pass vacuously").is_greater(0.05)

	var scene: Node = load("res://demo/main.tscn").instantiate()
	# Demo helper scripts use absolute /root/Main paths; drop them for this resource-only check
	# (the same trick test_cel_object.gd uses).
	scene.get_node("HUD/Label").set_script(null)
	scene.get_node("EditTool").set_script(null)
	add_child(scene)
	_nodes.append(scene)
	var env: Environment = (scene.get_node("WorldEnvironment") as WorldEnvironment).environment
	assert_bool(env.glow_enabled).is_true()
	assert_float(env.glow_hdr_threshold).override_failure_message(
		"glow threshold %f is at or below the sky's %f: the whole frame will bloom"
		% [env.glow_hdr_threshold, peak]).is_greater(peak)
	# glow_bloom lifts EVERY pixel regardless of the threshold, which is the other way to
	# wash the image out; the crack is supposed to be what glows.
	assert_float(env.glow_bloom).is_equal_approx(0.0, 0.001)
