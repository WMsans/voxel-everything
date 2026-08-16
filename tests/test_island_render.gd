extends GdUnitTestSuite

# Spec section 3's multi-target raymarching: an island is an ADDITIONAL march target whose
# hit competes with the terrain's on distance alone, so the two shade identically and
# occlude each other exactly.
#
# The island here is extracted from the terrain itself and then placed in the air above it,
# which makes the assertions unambiguous: everything the camera can see at that height is
# either the island or the sky.
#
# Deviation from the task brief: the brief's cell rows y=62..63 sit above the local terrain
# surface at this x/z (the generated hills dip to ~49.4 m, so those cells are mostly air and
# the centre ray misses). The tests use y=58..59 instead, which are fully underground and
# produce the solid 2x2x2 lump the assertions describe.

const SKY_UP := Color(0.25, 0.45, 0.85) # common.glslh's sky_color for dir.y = +1

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
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_physics()).is_true()
	for i in range(60):
		w.debug_stream_frame(Vector3(20.0, 56.0, 20.0))
	return w

func is_sky(c: Color) -> bool:
	# sky_color is a two-stop gradient; nothing the terrain or an island shades to sits on it.
	return absf(c.r - c.b) > 0.05 and c.b > c.r

func test_an_island_placed_in_the_air_is_hit_by_a_ray(timeout := 60000) -> void:
	var w := make_world()
	# Lift a 2x2x2-cell lump of rock 30 m above where it came from.
	var lift := Vector3(0.0, 30.0, 0.0)
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		lift)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]

	# Straight down through the island's centre from above it.
	var probe: Dictionary = w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).override_failure_message(
		"the ray passed through the island").is_true()
	var pos: Vector3 = probe["pos"]
	# It hit the island, not the terrain 30 m below it.
	assert_float(pos.y).is_greater(centre.y - 2.0)
	assert_bool(is_sky(probe["color"])).is_false()

func test_a_ray_beside_the_island_still_sees_the_sky(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	# Ten metres to the side of a 1.6 m lump, pointing up: nothing but sky.
	var c: Color = w.debug_raymarch_pixel(centre + Vector3(10, 0, 0), Vector3(0, 1, 0))
	assert_bool(is_sky(c)).override_failure_message(
		"a ray well clear of the island did not see the sky: %s" % c).is_true()

func test_the_island_occludes_the_terrain_behind_it(timeout := 60000) -> void:
	var w := make_world()
	var origin := Vector3(20.4, 90.0, 20.4)
	var before: Dictionary = w.debug_raymarch_probe(origin, Vector3(0, -1, 0))
	assert_bool(before["hit"]).is_true()
	var terrain_y: float = before["pos"].y

	# Place it once to learn where the lattice lands, then again to put it exactly between
	# the camera and the ground it just hit. (The hook's offset is a delta, because the
	# lattice origin is chosen by ve::plan_island_lattice and the caller cannot predict it.)
	var probe_place: Dictionary = w.debug_place_test_island(0, Vector3i(25, 58, 25),
		Vector3i(26, 59, 26), Vector3.ZERO)
	assert_bool(probe_place.get("ok", false)).is_true()
	var want := Vector3(20.4, 80.0, 20.4)
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		want - (probe_place["world_center"] as Vector3))
	assert_bool(d.get("ok", false)).is_true()
	assert_float((d["world_center"] as Vector3).distance_to(want)).is_less(0.01)

	var after: Dictionary = w.debug_raymarch_probe(origin, Vector3(0, -1, 0))
	assert_bool(after["hit"]).is_true()
	assert_float(after["pos"].y).override_failure_message(
		"the island did not occlude the terrain").is_greater(terrain_y + 5.0)

func test_a_rotated_island_is_hit_where_the_transform_puts_it(timeout := 60000) -> void:
	var w := make_world()
	# The same lump, rotated 90 degrees about y and moved. A rotation about the body origin
	# moves the lattice, so a ray that hit before must miss and one aimed at the new place
	# must hit -- which is what proves the inverse transform is applied and not skipped.
	var d: Dictionary = w.debug_place_test_island_rotated(0, Vector3i(25, 58, 25),
		Vector3i(28, 59, 26), Vector3(0.0, 30.0, 0.0), PI * 0.5)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]
	var probe: Dictionary = w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).is_true()
	assert_float(probe["pos"].y).is_greater(centre.y - 3.0)
	# The lump is 3 cells long on x and 2 on z; after the rotation its long axis is z, so a
	# ray 1.6 m out along x -- inside the UNROTATED extent, outside the rotated one -- misses.
	var side: Dictionary = w.debug_raymarch_probe(centre + Vector3(1.6, 6, 0), Vector3(0, -1, 0))
	assert_float(side["pos"].y if side["hit"] else -1000.0).is_less(centre.y - 5.0)

func test_a_cleared_slot_stops_being_marched(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	assert_bool(w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))["hit"]
		).is_true()
	w.debug_clear_test_island(0)
	var probe: Dictionary = w.debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	# The terrain 30 m below is still there, so this hits -- just not up here.
	assert_float(probe["pos"].y if probe["hit"] else -1000.0).is_less(centre.y - 20.0)
