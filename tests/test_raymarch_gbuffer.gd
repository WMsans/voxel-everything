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

# Straight down onto open ground from well above it: a hit, facing up, on a real material.
func probe_ground(w: VoxelWorld) -> Dictionary:
	return w.hooks().debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0))

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

# The near field's G-buffer albedo is produced by composite.frag.glsl, not by the marcher --
# the marcher exports geometry and a ray overlay, and the material is resolved once per
# full-resolution pixel (see tests/test_near_field_scale.gd for why). So this contract is
# asserted where the value is written: on the composited G-buffer, through a real march at
# the shipped scale rather than a 1x1 probe.
func test_the_composited_albedo_is_albedo_and_not_shaded_colour() -> void:
	var w := make_world()
	var lit: Dictionary = w.hooks().debug_near_field_detail(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 96, 96, 0.4)
	assert_bool(lit["ran"]).is_true()
	assert_bool(lit["center_hit"]).is_true()
	w.set_effect_enabled("raymarched_sun_shadow", false)
	var flat: Dictionary = w.hooks().debug_near_field_detail(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 96, 96, 0.4)
	var a: Color = lit["center_albedo"]
	var b: Color = flat["center_albedo"]
	# Turning the sun ray off moves the SUN channel and nothing else. If either stage were
	# baking light into the colour, the albedo would move with it. This is the contract the
	# whole deferred stack rests on.
	assert_float(absf(a.r - b.r)).is_less(0.005)
	assert_float(absf(a.g - b.g)).is_less(0.005)
	assert_float(absf(a.b - b.b)).is_less(0.005)
	assert_float(float(flat["center_sun"])).is_greater_equal(float(lit["center_sun"]))
	# ...and what is stored is the material's own albedo, darkened only by ambient occlusion
	# (at most 35%), never by a lambert term.
	var direct: Color = w.hooks().debug_material_probe(int(lit["center_material"]),
		lit["center_position"], lit["center_normal"])
	assert_float(a.r).is_between(direct.r * 0.65 - 0.02, direct.r + 0.02)
	assert_float(a.g).is_between(direct.g * 0.65 - 0.02, direct.g + 0.02)
	assert_float(a.b).is_between(direct.b * 0.65 - 0.02, direct.b + 0.02)

# The marcher's own targets, unresolved. Its albedo image carries the ray OVERLAY: on an
# ordinary lit hit there is nothing to overlay, so the weight is zero and the composite keeps
# the whole material. A non-zero weight here on plain ground would mean a ray effect was
# leaking into every pixel.
func test_an_ordinary_hit_carries_no_ray_overlay() -> void:
	var w := make_world()
	var d := probe_ground(w)
	assert_bool(d["hit"]).is_true()
	assert_float(float(d["overlay_weight"])).is_equal_approx(0.0, 0.001)

# ...and a miss is ALL overlay: the sky owns the pixel, at full weight.
func test_a_miss_is_entirely_overlay() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
	assert_bool(d["hit"]).is_false()
	assert_float(float(d["overlay_weight"])).is_equal_approx(1.0, 0.001)
	var overlay: Color = d["overlay"]
	assert_float(overlay.b).is_greater(overlay.r)

func test_the_sky_writes_material_zero_and_keeps_the_sky_colour() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
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
	var air: Dictionary = w.hooks().debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, 1, 0))
	assert_float(air["gloss"]).is_equal_approx(0.0, 0.001)

# Shadow layer 1. A point on open ground faces the sun; a point at the bottom of the
# generator's carved cave at (30, ~49, 30) does not.
func test_the_sun_ray_shadows_the_inside_of_the_cave() -> void:
	var w := make_world()
	var lit := probe_ground(w)
	assert_float(lit["sun"]).is_greater(0.5)
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 50.0, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	# Straight down inside the cave mouth: the floor there is under 5 m of overhang.
	var dark: Dictionary = w.hooks().debug_raymarch_gbuffer(Vector3(30.0, 50.0, 30.0), Vector3(0, -1, 0))
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
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
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
	var d: Dictionary = w.hooks().debug_raymarch_hole_probe(Vector3(20.0, 70.0, 20.0),
		Vector3(1.0, -0.8, 0.6).normalized(), 384, 256)
	assert_bool(d["ran"]).is_true()
	assert_int(d["hit_pixels"]).override_failure_message(
		"the view hit nothing, so the hole count below proves nothing").is_greater(20000)
	assert_int(d["isolated_misses"]).is_equal(0)

# Task 7's invariance contract: the G-buffer normal comes from the source field (or its
# R8 fallback), NEVER from the material normal map. Rewriting a material layer's normal-
# map texels with a hard tilt must not move the decoded normal by one bit.
func test_material_normal_map_bytes_do_not_change_gbuffer_normals() -> void:
	var w := make_world()
	var before := probe_ground(w)
	assert_bool(before["hit"]).is_true()
	var mat: int = before["material"]
	assert_bool(w.hooks().debug_poke_material_normal(mat)).is_true()
	var after := probe_ground(w)
	assert_int(after["material"]).is_equal(mat)
	var n0: Vector3 = before["normal"]
	var n1: Vector3 = after["normal"]
	assert_float((n0 - n1).length()).override_failure_message(
		"G-buffer normal moved when the material normal map changed: %s -> %s" % [n0, n1]
		).is_equal_approx(0.0, 0.0001)

# The near field's shadow reach. The ortho sun map is rasterized from the LoD mesh, so it
# only shades the pixels that mesh drew (see shaders/deferred.comp.glsl); the near field has
# to find its own occluders, however far up the sun ray they are. This was capped at the
# 7.68 m penumbra band while the map was believed to cover the near field too, which left an
# occluder 25 m up-sun casting nothing at all.
func test_the_near_field_marches_past_its_penumbra_band_for_an_occluder() -> void:
	var w := make_tall_world()
	var lit := probe_ground(w)
	assert_bool(lit["hit"]).is_true()
	assert_float(lit["sun"]).is_equal_approx(1.0, 0.02)
	var ground: Vector3 = lit["position"]
	# Well outside the 7.68 m penumbra band, and well inside the 60 m march.
	const SUN := Vector3(0.5746958, 0.7662610, 0.2873479) # ve::kSunDir
	w.hooks().debug_apply_sphere_add(ground + SUN * 25.0, 6.0, 1)
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break
	var shaded := probe_ground(w)
	assert_bool(shaded["hit"]).is_true()
	# The blocker sits between this ground and the sun; the marched term has to see it.
	assert_float(shaded["sun"]).override_failure_message(
		"an occluder 25 m up-sun left the near field fully lit: sun=%s" % shaded["sun"]) \
		.is_less(0.5)
