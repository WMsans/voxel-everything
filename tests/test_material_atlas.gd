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
	# Stream the terrain around the near-field probe origin so the texture-detail test
	# actually raymarches resident bricks (the brief's make_world omitted this settle).
	var _quiet := 0
	for i in range(120):
		_quiet = _quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if _quiet >= 6:
			break
	return w

func test_the_arrays_load_with_mips() -> void:
	var w := make_world()
	var d := w.hooks().debug_material_atlas_stats()
	assert_int(d["layers"]).is_greater_equal(4)
	assert_int(d["width"]).is_equal(512)
	assert_int(d["height"]).is_equal(512)
	# Mips are what make the far field resolve to an average instead of aliasing; without
	# them a 2 m tile at 2 km sparkles.
	assert_int(d["mipmaps"]).is_greater_equal(9)
	assert_bool(d["albedo_valid"]).is_true()
	assert_bool(d["surface_valid"]).is_true()

# The near field must now vary WITHIN one material. Before this task every grass pixel was
# exactly material_albedo(1); after it, two points 1 m apart on the same material differ.
func test_the_near_field_gains_texture_detail() -> void:
	var w := make_world()
	var origin := Vector3(20.0, 70.0, 20.0)
	var a: Color = w.hooks().debug_raymarch_pixel(origin, Vector3(0.05, -1.0, 0.0).normalized())
	var b: Color = w.hooks().debug_raymarch_pixel(origin, Vector3(-0.05, -1.0, 0.0).normalized())
	assert_bool(a.a > 0.0 or b.a > 0.0).override_failure_message(
		"neither probe hit the terrain").is_true()
	var diff := absf(a.r - b.r) + absf(a.g - b.g) + absf(a.b - b.b)
	assert_float(diff).override_failure_message(
		"two points on the same material shaded identically: no texture is being sampled"
		).is_greater(0.002)

# The albedo array's alpha used to carry a height map that no shader ever read. It now
# carries the glow mask. A material with no glow PNG packs flat 1.0 so the table's scalar
# still drives it uniformly -- that fallback is why "glow" and "glow mask" are separable.
func test_the_glow_mask_lands_in_the_albedo_alpha() -> void:
	var w := make_world()
	var d := w.hooks().debug_material_alpha_stats(4) # layer 4 == magma
	assert_bool(d.has("min")).override_failure_message(
		"debug_material_alpha_stats returned nothing for layer 4").is_true()
	# Cracks glow, raised stone does not: the mask must actually vary.
	assert_float(d["max"] - d["min"]).override_failure_message(
		"magma's glow mask is flat: the converter did not emit 04_glow.png"
		).is_greater(0.2)

func test_a_material_without_a_glow_png_packs_a_flat_mask() -> void:
	var w := make_world()
	var d := w.hooks().debug_material_alpha_stats(0) # layer 0 == grass_01, no glow art
	assert_float(d["min"]).is_equal_approx(1.0, 0.02)
	assert_float(d["max"]).is_equal_approx(1.0, 0.02)

# Sampling must fall back rather than read garbage when a material has no layer.
func test_an_unknown_material_falls_back_to_flat_albedo() -> void:
	var w := make_world()
	var c: Color = w.hooks().debug_material_probe(9999, Vector3(10.0, 51.0, 10.0), Vector3(0, 1, 0))
	assert_bool(c.r > 0.9 and c.g < 0.1 and c.b > 0.9).override_failure_message(
		"an out-of-range material id should shade error magenta, got %s" % c).is_true()
