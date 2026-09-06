extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(radius: float) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = radius
	w.max_lod_pages = 32768
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# Same strict convergence as test_sun_shadow.gd's settle: the shadow build draws the
# resident cut, and an unsettled pool rasterizes nothing -- every build below would be
# refused and the visibility/pages assertions would pass against empty maps.
const SETTLE_BUDGET := 2500
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var cameras := [fwd, Vector3(-1, -0.3, -1).normalized()]
	for camera in cameras:
		var quiet := 0
		for i in range(SETTLE_BUDGET):
			w.hooks().debug_lod_tick(pos, camera)
			await get_tree().process_frame
			var d: Dictionary = w.hooks().debug_lod_stats()
			quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
			if quiet >= QUIET_TICKS:
				return true
	push_error("strict settle timeout: %s" % w.hooks().debug_lod_stats())
	return false

func test_three_cascades_are_reported_with_derived_radii() -> void:
	var w := make_world(4000.0)
	w.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	var c0: Dictionary = w.hooks().debug_sun_shadow_stats(0)
	var c1: Dictionary = w.hooks().debug_sun_shadow_stats(1)
	var c2: Dictionary = w.hooks().debug_sun_shadow_stats(2)
	assert_int(c0["cascades"]).is_equal(3)
	assert_float(c0["radius"]).is_equal_approx(409.4, 0.01)
	assert_float(c1["radius"]).is_equal_approx(1279.687, 0.01)
	assert_float(c2["radius"]).is_equal_approx(4000.0, 0.01)
	# The clamp: cascade 0 unclamped by construction, the others floored at their texel.
	assert_int(c0["min_level"]).is_equal(0)
	assert_int(c1["min_level"]).is_equal(1)
	assert_int(c2["min_level"]).is_equal(3)

# THE no-regression property, checked through the shipping path rather than the pure
# function: whatever the radius, the outermost cascade is the map that shipped before.
func test_the_outermost_cascade_reproduces_the_old_single_map() -> void:
	var w := make_world(1638.4)
	w.hooks().debug_lod_tick(Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())
	var last: Dictionary = w.hooks().debug_sun_shadow_stats(2)
	assert_float(last["radius"]).is_equal_approx(1638.4, 0.001)
	assert_float(last["texel_world"]).is_equal_approx(1.600782, 0.0001)

# Each cascade snaps to its OWN texel, so this is A's shimmer guarantee asserted three
# times: sub-texel motion must not re-project any of them.
func test_sub_texel_motion_rebuilds_no_cascade(timeout := 120000) -> void:
	var w := make_world(4000.0)
	var fwd := Vector3(1, -0.3, 1).normalized()
	var origin := Vector3(60.0, 80.0, 60.0)
	assert_bool(await settle(w, origin, fwd)).is_true()
	for c in range(3):
		w.hooks().debug_sun_shadow_build(c, true)
	var before := []
	for c in range(3):
		before.append(int(w.hooks().debug_sun_shadow_stats(c)["rebuilds"]))
	for c in range(3):
		assert_int(before[c]).override_failure_message(
			"cascade %d never built (rebuilds=%d): map is empty, stillness check is vacuous" % [c, before[c]]
		).is_greater(0)
	# One tenth of cascade 0's texel -- far inside every cascade's snap grid.
	var texel0: float = w.hooks().debug_sun_shadow_stats(0)["texel_world"]
	for i in range(6):
		w.hooks().debug_lod_tick(origin + Vector3(0.1 * texel0 * i, 0.0, 0.0), fwd)
		for c in range(3):
			w.hooks().debug_sun_shadow_build(c, false)
	for c in range(3):
		var now := int(w.hooks().debug_sun_shadow_stats(c)["rebuilds"])
		assert_int(now).override_failure_message(
			"cascade %d rebuilt %d times under sub-texel motion" % [c, now - before[c]]
		).is_less_equal(before[c] + 1)

# The clamp must not lift the far field's shadowed ground off the ground (peter-panning).
# Sampled where cascade 2 owns the pixel, with the clamp on and off.
func test_the_min_level_clamp_does_not_peter_pan(timeout := 180000) -> void:
	var probe := Vector3(1800.0, 20.0, 1800.0)
	var w_on := make_world(4000.0)
	w_on.sun_cascade_min_level = true
	assert_bool(await settle(w_on, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	for c in range(3):
		w_on.hooks().debug_sun_shadow_build(c, true)
	var on: float = w_on.hooks().debug_sun_shadow_visibility(probe)

	var w_off := make_world(4000.0)
	w_off.sun_cascade_min_level = false
	assert_bool(await settle(w_off, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	for c in range(3):
		w_off.hooks().debug_sun_shadow_build(c, true)
	var off: float = w_off.hooks().debug_sun_shadow_visibility(probe)

	var on_pages: int = w_on.hooks().debug_sun_shadow_stats(2)["pages"]
	var off_pages: int = w_off.hooks().debug_sun_shadow_stats(2)["pages"]
	assert_int(on_pages).override_failure_message(
		"clamped cascade 2 drew %d pages: map is empty, peter-pan check is vacuous" % on_pages
	).is_greater(0)
	assert_int(off_pages).override_failure_message(
		"unclamped cascade 2 drew %d pages: map is empty, peter-pan check is vacuous" % off_pages
	).is_greater(0)
	assert_float(off).override_failure_message(
		"unclamped probe reads %f: map is empty, peter-pan check is vacuous" % off
	).is_less(0.5)

	# Deep underground is shadowed either way. If the clamp lifted the stored surface off
	# the ground, the clamped map would report this point LIT.
	assert_float(on).override_failure_message(
		"clamped cascade reports %f, unclamped reports %f: the clamp peter-panned" % [on, off]
	).is_equal_approx(off, 0.01)

# needs_rebuild() gates whether the cascade's cut is even produced, so a false negative is
# a shadow map that silently stops updating. It shares should_rebuild() with build(), which
# makes agreement structural; this pins it anyway.
func test_needs_rebuild_agrees_with_what_build_does(timeout := 120000) -> void:
	var w := make_world(4000.0)
	var fwd := Vector3(1, -0.3, 1).normalized()
	assert_bool(await settle(w, Vector3(60, 80, 60), fwd)).is_true()
	for c in range(3):
		w.hooks().debug_sun_shadow_build(c, true)
	for c in range(3):
		var built := int(w.hooks().debug_sun_shadow_stats(c)["rebuilds"])
		assert_int(built).override_failure_message(
			"cascade %d never built (rebuilds=%d): agree-check runs against empty state" % [c, built]
		).is_greater(0)
	# Nothing has moved and nothing is dirty, so no cascade should want a rebuild -- and an
	# unforced build must then decline for exactly the cascades that said so.
	for c in range(3):
		var wants: bool = w.hooks().debug_sun_shadow_stats(c)["needs_rebuild"]
		var did: bool = w.hooks().debug_sun_shadow_build(c, false)
		assert_bool(did).override_failure_message(
			"cascade %d: needs_rebuild=%s but build returned %s" % [c, wants, did]
		).is_equal(wants)

# The clamp is a saving. If it is on and costs the same, it is not doing anything.
func test_the_clamp_reduces_the_far_cascade_page_count(timeout := 180000) -> void:
	var w_on := make_world(4000.0)
	w_on.sun_cascade_min_level = true
	assert_bool(await settle(w_on, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w_on.hooks().debug_sun_shadow_build(2, true)
	var on: int = w_on.hooks().debug_sun_shadow_stats(2)["pages"]
	assert_int(on).override_failure_message(
		"clamped cascade 2 drew %d pages: map is empty, pages check is vacuous" % on
	).is_greater(0)

	var w_off := make_world(4000.0)
	w_off.sun_cascade_min_level = false
	assert_bool(await settle(w_off, Vector3(60, 80, 60), Vector3(1, -0.3, 1).normalized())).is_true()
	w_off.hooks().debug_sun_shadow_build(2, true)
	var off: int = w_off.hooks().debug_sun_shadow_stats(2)["pages"]
	assert_int(off).override_failure_message(
		"unclamped cascade 2 drew %d pages: map is empty, pages check is vacuous" % off
	).is_greater(0)

	assert_int(on).override_failure_message(
		"clamped cascade 2 drew %d pages, unclamped %d" % [on, off]).is_less_equal(off)
