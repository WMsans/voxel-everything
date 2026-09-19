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
	w.stream_radius_m = 1300.0 # the walk needs whole in-radius sibling sets (L6 spans
	# 819 m); this suite's cameras need ~1210 m, so 1300 is the floor here
	# 32768, the shipped default (LodSystem::max_lod_pages_): 16384 was enough only while
	# the walk stalled on out-of-radius siblings and drew coarse roots instead of the far
	# field; a complete cut for this camera needs ~19k pages.
	w.max_lod_pages = 32768
	w.near_field_scale = 1.0 # debug_seam_probe reads the marcher's hitpos per full-resolution pixel
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

const SETTLE_BUDGET := 6000
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d: Dictionary = w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# Drive the brick streamer to a quiet streak (the seam probe's internal 120-frame budget
# is a settle-time state once trees demand more funding ticks).
func settle_stream(w: VoxelWorld, pos: Vector3) -> bool:
	var quiet := 0
	for i in range(2000):
		var actions := w.hooks().debug_stream_frame(pos)
		await get_tree().process_frame
		quiet = quiet + 1 if actions == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# Trees recalibration (Task 7, ruling R9): mirror of test_lod_seam.gd -- the unclaimed
# reading is taken at STEADY STATE (consecutive equal probes), not at the settle-time
# peak, because with trunks the first probe reads 18/264 and the plateau is 13/265.
func probe_band_at_steady_state(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> Dictionary:
	await settle_stream(w, pos)
	var d: Dictionary = w.hooks().debug_seam_probe(pos, fwd, 256, 144)
	for r in range(12):
		for i in range(30):
			w.hooks().debug_lod_tick(pos, fwd)
			await get_tree().process_frame
		var d2: Dictionary = w.hooks().debug_seam_probe(pos, fwd, 256, 144)
		if int(d2["neither"]) == int(d["neither"]) and int(d2["band_pixels"]) == int(d["band_pixels"]):
			return d2
		d = d2
	return d

# The far field must describe, not shade. A LoD pixel has to carry a real material id and a
# unit normal, or the deferred pass has nothing to light it with.
func test_far_field_pixels_carry_a_material_and_a_unit_normal(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 200.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d: Dictionary = w.hooks().debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	assert_float(d["material_coverage"]).override_failure_message(
		"no far-field pixel wrote a material id").is_greater(0.2)
	assert_float(d["worst_normal_length_error"]).override_failure_message(
		"a far-field normal did not survive the oct pack").is_less(0.02)
	assert_float(d["gloss_max"]).is_between(0.0, 1.0)

# Spec section 7: "the near/far seam is mathematically invisible". With one lighting stack
# it is provable: turn the sun map and every screen-space effect off, and the two fields on
# either side of the band must land within a band's worth of each other.
func test_the_two_fields_light_identically_across_the_band(timeout := 180000) -> void:
	var w := make_world()
	w.quality_tier = 1 # outlines and the raymarched sun ray only; no screen-space passes
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d: Dictionary = await probe_band_at_steady_state(w, pos, fwd)
	# Preserve the established test_lod_seam.gd seam contract, re-derived under trees
	# (Task 7, ruling R9) from the STEADY STATE: the persistent unclaimed plateau is 13 of
	# 265 (4.9%), so the bar is band_pixels / 16 (6.25%) -- see the recalibration note in
	# test_lod_seam.gd. The documented stall-pathological 7.3% (>= 19 pixels of this band,
	# and a stalled lineage never recovers into the plateau) still clears the bar with
	# margin and fails the assertion. Double claims remain an exact invariant, and the
	# band must be non-vacuously measured.
	assert_int(d["both"]).is_equal(0)
	assert_int(d["neither"]).override_failure_message(
			"%d of %d band pixels were claimed by neither field (steady state)" % [d["neither"], d["band_pixels"]]
			).is_less_equal(int(d["band_pixels"] / 16))
	assert_int(d["band_pixels"]).is_greater(50)

func test_the_lod_raster_no_longer_shades(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 200.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d: Dictionary = w.hooks().debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	# The far field writes fully-lit sun visibility: shadowing it is the sun map's job, in
	# the deferred pass, not the raster's.
	assert_float(d["sun_min"]).is_equal_approx(1.0, 0.01)
	assert_float(d["sun_max"]).is_equal_approx(1.0, 0.01)

# The far field resolves its material through the same pair of calls the near field does
# (see the comment at the top of shaders/composite.frag.glsl), so it must pick up the
# material normal map too -- and it needs it more: a LoD quad's normal is flat across the
# whole quad, so without the map a distant hillside is one unbroken facet.
func test_the_material_normal_map_shapes_the_far_field_shading_normal(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 200.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var before: Dictionary = w.hooks().debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	assert_float(before["material_coverage"]).is_greater(0.2)
	# Which material a page carries is the walk's business, so poke every layer.
	for layer in range(16):
		assert_bool(w.hooks().debug_poke_material_normal(layer)).is_true()
	var after: Dictionary = w.hooks().debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	assert_float(after["material_coverage"]).is_greater(0.2)
	assert_float(after["worst_normal_length_error"]).override_failure_message(
		"a perturbed far-field normal did not survive the oct pack").is_less(0.02)
	var n0: Vector3 = before["normal_mean"]
	var n1: Vector3 = after["normal_mean"]
	assert_float((n0 - n1).length()).override_failure_message(
		"the material normal map did not move the far field: %s -> %s" % [n0, n1]
		).is_greater(0.05)
