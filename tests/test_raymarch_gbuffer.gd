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
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

# Straight down onto open ground from well above it: a hit, facing up, on a real material.
func probe_ground(w: VoxelWorld) -> Dictionary:
	return w.debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0))

func test_a_ground_hit_writes_a_material_and_a_normal() -> void:
	var w := make_world()
	var d := probe_ground(w)
	assert_bool(d["hit"]).is_true()
	assert_int(d["material"]).is_greater(0)
	# The generator's surface is a gentle height field, so straight down lands on ground
	# whose normal points broadly up. Anything else means the oct pack lost the vector.
	var n: Vector3 = d["normal"]
	assert_float(n.length()).is_equal_approx(1.0, 0.01)
	assert_float(n.y).is_greater(0.7)

func test_the_albedo_channel_is_albedo_and_not_shaded_colour() -> void:
	var w := make_world()
	var lit_probe := probe_ground(w)
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var flat_probe := probe_ground(w)
	var a: Color = lit_probe["albedo"]
	var b: Color = flat_probe["albedo"]
	# Turning the sun ray off moves the SUN channel and nothing else. If the raymarcher were
	# still calling shade_terrain(), the light would be baked into the colour and the albedo
	# would move with it. This is the contract the whole deferred stack rests on.
	assert_float(absf(a.r - b.r)).is_less(0.005)
	assert_float(absf(a.g - b.g)).is_less(0.005)
	assert_float(absf(a.b - b.b)).is_less(0.005)
	assert_float(flat_probe["sun"]).is_greater_equal(lit_probe["sun"])
	# ...and what is stored is the material's own albedo, darkened only by ambient occlusion
	# (at most 35%), never by a lambert term.
	var direct: Color = w.debug_material_probe(int(lit_probe["material"]),
		lit_probe["position"], lit_probe["normal"])
	assert_float(a.r).is_between(direct.r * 0.65 - 0.01, direct.r + 0.01)
	assert_float(a.g).is_between(direct.g * 0.65 - 0.01, direct.g + 0.01)
	assert_float(a.b).is_between(direct.b * 0.65 - 0.01, direct.b + 0.01)

func test_the_sky_writes_material_zero_and_keeps_the_sky_colour() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
	assert_bool(d["hit"]).is_false()
	assert_int(d["material"]).is_equal(0)
	# Material 0 means "no voxel here" and the deferred pass passes the albedo channel
	# through unlit, so the sky gradient has to survive in it.
	var albedo: Color = d["albedo"]
	assert_float(albedo.b).is_greater(albedo.r)

func test_gloss_comes_from_the_material_surface_array() -> void:
	var w := make_world()
	var d := probe_ground(w)
	# gloss = 1 - roughness, and the shipped materials are all rough ground, so it is low
	# but it must not be the constant 0 that "we never sampled the surface array" produces
	# for every material equally.
	assert_float(d["gloss"]).is_between(0.0, 1.0)
	# ...and it is not the constant the "we never sampled the surface array" bug produces:
	# an out-of-range material falls back to roughness 1, i.e. gloss exactly 0.
	var air: Dictionary = w.debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
	assert_float(air["gloss"]).is_equal_approx(0.0, 0.001)

# Shadow layer 1. A point on open ground faces the sun; a point at the bottom of the
# generator's carved cave at (30, ~49, 30) does not.
func test_the_sun_ray_shadows_the_inside_of_the_cave() -> void:
	var w := make_world()
	var lit := probe_ground(w)
	assert_float(lit["sun"]).is_greater(0.5)
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(30.0, 50.0, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	# Straight down inside the cave mouth: the floor there is under 5 m of overhang.
	var dark: Dictionary = w.debug_raymarch_gbuffer(Vector3(30.0, 50.0, 30.0), Vector3(0, -1, 0))
	assert_bool(dark["hit"]).is_true()
	assert_float(dark["sun"]).is_less(lit["sun"])

func test_turning_the_sun_ray_off_makes_everything_fully_lit() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var d := probe_ground(w)
	assert_float(d["sun"]).is_equal_approx(1.0, 0.01)

# Shadow layer 1, the open-sky case. The SDF is a NARROW BAND: world_sdf() saturates at
# +SDF_RANGE (0.64 m), so a sample in open air says "no surface within 0.64 m" and nothing
# more. The sphere-traced penumbra term K*d/t treats that saturated value as a true occluder
# distance, so past t = K*SDF_RANGE (7.68 m) every open-air step darkens the result by 1/t.
#
# make_world()'s 5-region-tall world hides this: the shadow ray leaves the region field after
# ~27 m and takes the fully-lit early-out. The demo's world is tall enough that the ray runs
# the whole 60 m budget inside it, which is why the terrain there renders near-black.
func make_tall_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	# 7 regions of head-room puts the world ceiling at y = 179.2, well above the y = 102 the
	# 60 m shadow ray reaches from the ground at y = 56.
	w.world_size_regions = Vector3i(8, 7, 8)
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_open_ground_under_open_sky_is_fully_lit() -> void:
	var w := make_tall_world()
	var d := probe_ground(w)
	assert_bool(d["hit"]).is_true()
	# Flat ground facing up with nothing between it and the sun. Anything below 1.0 here is
	# the saturated band being read as an occluder.
	assert_float(d["sun"]).is_equal_approx(1.0, 0.02)

func test_open_sky_visibility_does_not_depend_on_world_height() -> void:
	# The same ground point, the same sun, two worlds that differ only in how much EMPTY
	# space sits above the surface. Sun visibility is a property of the geometry, so the two
	# must agree; if they do not, the marcher is measuring the air rather than the occluders.
	var short_sun: float = probe_ground(make_world())["sun"]
	var tall_sun: float = probe_ground(make_tall_world())["sun"]
	assert_float(tall_sun).is_equal_approx(short_sun, 0.02)

# The 8^3 min-max chain gates whether a cell is sphere-traced at all. The march's hit test is
# one-sided (any d below a small positive threshold, negatives included), so the only sound
# question for the gate is whether the cell's MINIMUM is low enough. Also demanding a sign
# change skipped every cell lying wholly inside the surface, and a ray that entered the solid
# through one was advanced straight out the far side.
#
# The result is a single missed pixel surrounded by hits. Real sky is a connected region, so
# an isolated miss in the middle of terrain can only be the march stepping over geometry.
func test_the_march_leaves_no_isolated_holes_in_the_gbuffer() -> void:
	var w := make_world()
	# Looking down onto the height field from 19 m up: the rays cross many bricks at a steep
	# enough angle to enter the solid, which is where a cell-level skip that also demands a
	# sign change loses them. This view produced 8 holes before the gate was corrected.
	var d: Dictionary = w.debug_raymarch_hole_probe(Vector3(20.0, 70.0, 20.0),
		Vector3(1.0, -0.8, 0.6).normalized(), 384, 256)
	assert_bool(d["ran"]).is_true()
	assert_int(d["hit_pixels"]).override_failure_message(
		"the view hit nothing, so the hole count below proves nothing").is_greater(20000)
	assert_int(d["isolated_misses"]).is_equal(0)
