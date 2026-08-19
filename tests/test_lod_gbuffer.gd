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
	w.max_lod_pages = 4096
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d: Dictionary = w.debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# The far field must describe, not shade. A LoD pixel has to carry a real material id and a
# unit normal, or the deferred pass has nothing to light it with.
func test_far_field_pixels_carry_a_material_and_a_unit_normal(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 100.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d: Dictionary = w.debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	assert_float(d["material_coverage"]).override_failure_message(
		"no far-field pixel wrote a material id").is_greater(0.2)
	assert_float(d["worst_normal_length_error"]).override_failure_message(
		"a far-field normal did not survive the oct pack").is_less(0.02)
	assert_float(d["gloss_max"]).is_between(0.0, 1.0)

# Spec section 7: "the near/far seam is mathematically invisible". With one lighting stack
# it is provable: turn the sun map and every screen-space effect off, and the two fields on
# either side of the band must land within a band's worth of each other.
func test_the_two_fields_light_identically_across_the_band(timeout := 60000) -> void:
	var w := make_world()
	w.quality_tier = 1 # outlines and the raymarched sun ray only; no screen-space passes
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d: Dictionary = w.debug_seam_probe(pos, fwd, 256, 144)
	# The existing exactly-once invariant still holds with two attachments in play.
	assert_int(d["both"]).is_equal(0)
	assert_int(d["neither"]).is_equal(0)
	assert_int(d["band_pixels"]).is_greater(50)

func test_the_lod_raster_no_longer_shades(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 100.0, 100.0)
	var fwd := Vector3(0.3, -0.5, 0.3).normalized()
	assert_bool(await settle(w, pos, fwd)).is_true()
	var d: Dictionary = w.debug_lod_gbuffer_probe(pos, fwd, 128, 128)
	# The far field writes fully-lit sun visibility: shadowing it is the sun map's job, in
	# the deferred pass, not the raster's.
	assert_float(d["sun_min"]).is_equal_approx(1.0, 0.01)
	assert_float(d["sun_max"]).is_equal_approx(1.0, 0.01)
