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

# The normal map was packed into the surface array's RG from the day the atlas was written
# and read by nothing: every shading path took the geometric normal and stopped there, so no
# terrain surface had any relief below the size of a voxel. It is applied where the material
# is resolved -- composite.frag.glsl for the near field, lod.frag.glsl for the far one -- so
# the normal the deferred pass lights carries the map and the MARCHER's own normal (which
# traversal and the shadow ray bias read) stays geometric. See
# tests/test_raymarch_gbuffer.gd for the other half of that split.
#
# debug_poke_material_normal rewrites a layer's RG with the hardest tilt the format holds,
# so a shading normal that follows the map cannot stay where it was.
func test_the_material_normal_map_shapes_the_near_field_shading_normal() -> void:
	var w := make_world()
	var pos := Vector3(20.0, 75.0, 20.0)
	var fwd := Vector3(0.0, -1.0, 0.0)
	var before: Dictionary = w.hooks().debug_near_field_detail(pos, fwd, 64, 64, 1.0)
	assert_bool(before["ran"]).is_true()
	assert_bool(before["center_hit"]).override_failure_message(
		"the probe ray missed the terrain, so the normal below proves nothing").is_true()
	var mat: int = before["center_material"]
	assert_int(mat).is_greater(0)
	# Layer i serves material id i + 1.
	assert_bool(w.hooks().debug_poke_material_normal(mat - 1)).is_true()
	var after: Dictionary = w.hooks().debug_near_field_detail(pos, fwd, 64, 64, 1.0)
	assert_int(after["center_material"]).is_equal(mat)
	var n0: Vector3 = before["center_normal"]
	var n1: Vector3 = after["center_normal"]
	assert_float(n1.length()).override_failure_message(
		"the shading normal is not unit length: %s" % n1).is_equal_approx(1.0, 0.02)
	assert_float((n0 - n1).length()).override_failure_message(
		"the material normal map did not move the shading normal: %s -> %s" % [n0, n1]
		).is_greater(0.1)

# A flat map must return the geometric normal EXACTLY, or every surface acquires a permanent
# tilt from its own texture. The whiteout blend has that property by construction; this pins
# it, because it is the property that makes a strength of 1.0 safe to ship.
func test_a_flat_normal_map_leaves_the_shading_normal_alone() -> void:
	var w := make_world()
	var pos := Vector3(20.0, 75.0, 20.0)
	var fwd := Vector3(0.0, -1.0, 0.0)
	var before: Dictionary = w.hooks().debug_near_field_detail(pos, fwd, 64, 64, 1.0)
	assert_bool(before["center_hit"]).is_true()
	var mat: int = before["center_material"]
	assert_bool(w.hooks().debug_flatten_material_normal(mat - 1)).is_true()
	var after: Dictionary = w.hooks().debug_near_field_detail(pos, fwd, 64, 64, 1.0)
	var n_flat: Vector3 = after["center_normal"]
	# The marcher's own normal for the SAME pixel, so the comparison is not against a
	# neighbouring ray's surface.
	var n_geo: Vector3 = after["center_geometric_normal"]
	assert_float((n_flat - n_geo).length()).override_failure_message(
		"a flat normal map tilted the surface: geometric %s, shaded %s" % [n_geo, n_flat]
		).is_less(0.02)

# The poke tests prove the map is WIRED. This one proves the SHIPPED ART says something
# through it: probe every table material at mip 0 (zero gradients, no geometry, no render
# path) and require the normal it produces to be a real tilt off the geometric one. A future
# change that sets MATERIAL_NORMAL_STRENGTH to zero, drops the RG channels from pack_layer,
# or ships flat normal art lands here rather than in a screenshot.
#
# ~10 degrees is what assets/materials actually carry (their normal maps have a per-channel
# standard deviation around 20/255); the bound is set well under that so ordinary art
# revisions do not trip it.
func test_every_material_normal_map_carries_real_relief() -> void:
	var w := make_world()
	var up := Vector3(0, 1, 0)
	for entry in w.material_table():
		var mat: int = entry["id"]
		var tilt := 0.0
		var samples := 0
		for i in range(100):
			# An irrational-ish stride so the sample grid never lands on one texel row.
			var p := Vector3(float(i % 10) * 0.0731, 0.0, float(i / 10) * 0.0917)
			var sn: Vector3 = w.hooks().debug_material_normal_probe(mat, p, up)
			assert_float(sn.length()).override_failure_message(
				"material %d probed a non-unit shading normal %s" % [mat, sn]
				).is_equal_approx(1.0, 0.02)
			tilt += 1.0 - sn.dot(up)
			samples += 1
		var mean := tilt / float(samples)
		assert_float(mean).override_failure_message(
			"material %s (id %d) shades perfectly flat: its normal map reaches nothing"
			% [entry["name"], mat]).is_greater(0.002) # ~3.6 degrees
