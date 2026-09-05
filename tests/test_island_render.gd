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
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	for i in range(60):
		w.hooks().debug_stream_frame(Vector3(20.0, 56.0, 20.0))
	return w

func is_sky(c: Color) -> bool:
	# sky_color is a two-stop gradient; nothing the terrain or an island shades to sits on it.
	return absf(c.r - c.b) > 0.05 and c.b > c.r

func test_an_island_placed_in_the_air_is_hit_by_a_ray(timeout := 60000) -> void:
	var w := make_world()
	# Lift a 2x2x2-cell lump of rock 30 m above where it came from.
	var lift := Vector3(0.0, 30.0, 0.0)
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		lift)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]

	# Straight down through the island's centre from above it.
	var probe: Dictionary = w.hooks().debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).override_failure_message(
		"the ray passed through the island").is_true()
	var pos: Vector3 = probe["pos"]
	# It hit the island, not the terrain 30 m below it.
	assert_float(pos.y).is_greater(centre.y - 2.0)
	assert_bool(is_sky(probe["color"])).is_false()

# Regression: the shader strides the SHARED authoritative SDF/material buffers with the
# island's VOLUME slot, never with its atlas slot. Real bodies get the two from different
# pools (32 atlas slots, 64 volume slots) and they diverge for good once a merged body pins
# its volume slot while its atlas slot is freed, so an island placed at atlas 0 / volume 3
# used to render whatever lived at volume 0 -- another body's geometry, with its own normals
# on top. Every other fixture here places an island with both slots equal, which is exactly
# why this went unnoticed; this one forces them apart.
func test_an_island_renders_its_own_volume_not_its_atlas_slots(timeout := 60000) -> void:
	var w := make_world()
	var lift := Vector3(0.0, 30.0, 0.0)
	# Decoy at volume slot 0, 40 m away: the bytes the buggy stride would have read.
	var decoy: Dictionary = w.hooks().debug_place_test_island_rotated(1, Vector3i(35, 58, 35),
		Vector3i(36, 59, 36), lift, 0.0, 0)
	assert_bool(decoy.get("ok", false)).override_failure_message(str(decoy)).is_true()
	# Subject at atlas slot 0, volume slot 3.
	var d: Dictionary = w.hooks().debug_place_test_island_rotated(0, Vector3i(25, 58, 25),
		Vector3i(26, 59, 26), lift, 0.0, 3)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]

	var probe: Dictionary = w.hooks().debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).override_failure_message(
		"the ray passed through the island at atlas 0 / volume 3").is_true()
	assert_float(probe["pos"].y).override_failure_message(
		"hit %s is not on the island at %s -- the shader read the wrong volume slot"
			% [str(probe["pos"]), str(centre)]).is_greater(centre.y - 2.0)
	assert_bool(is_sky(probe["color"])).is_false()

	# The decoy is still where it belongs, so the subject did not simply overwrite it.
	var decoy_centre: Vector3 = decoy["world_center"]
	var decoy_probe: Dictionary = w.hooks().debug_raymarch_probe(
		decoy_centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(decoy_probe["hit"]).override_failure_message(
		"the decoy island at atlas 1 / volume 0 stopped rendering").is_true()
	assert_float(decoy_probe["pos"].y).is_greater(decoy_centre.y - 2.0)

func test_a_ray_beside_the_island_still_sees_the_sky(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	# Ten metres to the side of a 1.6 m lump, pointing up: nothing but sky.
	var c: Color = w.hooks().debug_raymarch_pixel(centre + Vector3(10, 0, 0), Vector3(0, 1, 0))
	assert_bool(is_sky(c)).override_failure_message(
		"a ray well clear of the island did not see the sky: %s" % c).is_true()

func test_the_island_occludes_the_terrain_behind_it(timeout := 60000) -> void:
	var w := make_world()
	var origin := Vector3(20.4, 90.0, 20.4)
	var before: Dictionary = w.hooks().debug_raymarch_probe(origin, Vector3(0, -1, 0))
	assert_bool(before["hit"]).is_true()
	var terrain_y: float = before["pos"].y

	# Place it once to learn where the lattice lands, then again to put it exactly between
	# the camera and the ground it just hit. (The hook's offset is a delta, because the
	# lattice origin is chosen by ve::plan_island_lattice and the caller cannot predict it.)
	var probe_place: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25),
		Vector3i(26, 59, 26), Vector3.ZERO)
	assert_bool(probe_place.get("ok", false)).is_true()
	var want := Vector3(20.4, 80.0, 20.4)
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		want - (probe_place["world_center"] as Vector3))
	assert_bool(d.get("ok", false)).is_true()
	assert_float((d["world_center"] as Vector3).distance_to(want)).is_less(0.01)

	var after: Dictionary = w.hooks().debug_raymarch_probe(origin, Vector3(0, -1, 0))
	assert_bool(after["hit"]).is_true()
	assert_float(after["pos"].y).override_failure_message(
		"the island did not occlude the terrain").is_greater(terrain_y + 5.0)

func test_a_rotated_island_is_hit_where_the_transform_puts_it(timeout := 60000) -> void:
	var w := make_world()
	# The same lump, rotated 90 degrees about y and moved. A rotation about the body origin
	# moves the lattice, so a ray that hit before must miss and one aimed at the new place
	# must hit -- which is what proves the inverse transform is applied and not skipped.
	var d: Dictionary = w.hooks().debug_place_test_island_rotated(0, Vector3i(25, 58, 25),
		Vector3i(28, 59, 26), Vector3(0.0, 30.0, 0.0), PI * 0.5)
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	var centre: Vector3 = d["world_center"]
	var probe: Dictionary = w.hooks().debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	assert_bool(probe["hit"]).is_true()
	assert_float(probe["pos"].y).is_greater(centre.y - 3.0)
	# The lump is 3 cells long on x and 2 on z; after the rotation its long axis is z, so a
	# ray 1.6 m out along x -- inside the UNROTATED extent, outside the rotated one -- misses.
	var side: Dictionary = w.hooks().debug_raymarch_probe(centre + Vector3(1.6, 6, 0), Vector3(0, -1, 0))
	assert_float(side["pos"].y if side["hit"] else -1000.0).is_less(centre.y - 5.0)

func test_a_cleared_slot_stops_being_marched(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 58, 25), Vector3i(26, 59, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	assert_bool(w.hooks().debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))["hit"]
		).is_true()
	w.hooks().debug_clear_test_island(0)
	var probe: Dictionary = w.hooks().debug_raymarch_probe(centre + Vector3(0, 6, 0), Vector3(0, -1, 0))
	# The terrain 30 m below is still there, so this hits -- just not up here.
	assert_float(probe["pos"].y if probe["hit"] else -1000.0).is_less(centre.y - 20.0)

# Spec section 3's tiled target culling. The mask is one uint per 16x16 tile, bit i set when
# island i's world AABB may cover that tile. Correctness has one direction that matters: a
# tile that CAN see the island must have the bit, or the island vanishes for those pixels.
# An extra bit only costs a march.
const TAN_X := 0.6
const TAN_Y := 0.4

func test_the_tile_mask_marks_the_tiles_the_island_covers(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]

	# Look straight at it from 20 m away, at 128x128 -> an 8x8 tile grid.
	var eye := centre + Vector3(0.0, 0.0, 20.0)
	var mask: PackedInt32Array = w.hooks().debug_island_tile_mask(eye, Vector3(0, 0, -1),
		TAN_X, TAN_Y, 128, 128)
	assert_int(mask.size()).is_equal(64)
	var set_tiles := 0
	for m in mask:
		if (m & 1) != 0:
			set_tiles += 1
	# A 1.6 m lump 20 m away subtends a small part of the view: some tiles, not all of them.
	assert_int(set_tiles).is_greater(0)
	assert_int(set_tiles).is_less(64)
	# The island sits at NDC (0, 0), which is the shared corner of the four middle tiles;
	# whichever of them the inclusive bounds hand it to, at least one must be marked.
	var middle: int = mask[3 * 8 + 3] | mask[3 * 8 + 4] | mask[4 * 8 + 3] | mask[4 * 8 + 4]
	assert_int(middle & 1).is_not_equal(0)

func test_an_island_behind_the_camera_marks_nothing(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]
	var mask: PackedInt32Array = w.hooks().debug_island_tile_mask(centre + Vector3(0, 0, 20),
		Vector3(0, 0, 1), TAN_X, TAN_Y, 128, 128)
	for m in mask:
		assert_int(m & 1).is_equal(0)

func test_an_island_the_camera_is_inside_marks_every_tile(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	# Inside the lattice box, where the projection of its corners says nothing useful: the
	# pass must fail SAFE and mark everything rather than culling the island away.
	var mask: PackedInt32Array = w.hooks().debug_island_tile_mask(d["world_center"], Vector3(0, 0, -1),
		TAN_X, TAN_Y, 128, 128)
	for m in mask:
		assert_int(m & 1).is_not_equal(0)

func test_a_cleared_slot_is_never_marked(timeout := 60000) -> void:
	var w := make_world()
	var a: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(a.get("ok", false)).is_true()
	# A second island in slot 3, in the same place, then killed. Its BYTES stay in the atlas
	# by design (clear_slot only zeroes the descriptor), so this is the test that the
	# descriptor -- not the bytes -- is what decides whether a slot is marched.
	var b: Dictionary = w.hooks().debug_place_test_island(3, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(b.get("ok", false)).is_true()
	var eye: Vector3 = (a["world_center"] as Vector3) + Vector3(0, 0, 20)
	var both: PackedInt32Array = w.hooks().debug_island_tile_mask(eye, Vector3(0, 0, -1), TAN_X,
		TAN_Y, 128, 128)
	var saw_three := false
	for m in both:
		if (m & 8) != 0:
			saw_three = true
	assert_bool(saw_three).is_true()

	w.hooks().debug_clear_test_island(3)
	var after: PackedInt32Array = w.hooks().debug_island_tile_mask(eye, Vector3(0, 0, -1), TAN_X,
		TAN_Y, 128, 128)
	var live_zero := false
	for m in after:
		assert_int(m & 8).is_equal(0)
		if (m & 1) != 0:
			live_zero = true
	assert_bool(live_zero).is_true()

func test_non_multiple_of_16_viewport_marks_the_tile_that_contains_the_island(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]

	# 130x90 -> a 9x6 tile grid whose last column/row are partial. Put the island near the
	# right edge, with its leftmost projected corner in column 7. The OLD padded denominator
	# (9*16=144) made column 7's NDC bound end at 128/144 = 0.7778 even though real pixels in
	# that column extend to 128/130 = 0.9692. With the real 130-wide denominator the tile
	# containing that leftmost visible corner is marked; with the padded denominator it would
	# be handed to column 8 and the pixels that can actually see it would not have the bit.
	const VIEW_W := 130
	const VIEW_H := 90
	var tiles_x: int = ceili(float(VIEW_W) / 16.0)
	var tiles_y: int = ceili(float(VIEW_H) / 16.0)
	assert_int(tiles_x).is_equal(9)
	assert_int(tiles_y).is_equal(6)

	# The island's centre is at NDC x ~1.0 (just off the right edge of the 130-wide view),
	# but its AABB still reaches left into column 7. Probe the tile at the AABB's leftmost
	# projected corner: with the padded denominator that corner (ndc ~0.805) lies beyond
	# column 7's old bound (0.778), so the tile is dropped; with the real denominator it is
	# correctly inside column 7.
	const OFFSET_X := 12.0
	const DIST := 20.0
	const HALF_SPAN := 1.575 # (kIslandDim - 1) * kIslandVoxelFine / 2
	var eye := centre + Vector3(-OFFSET_X, 0.0, DIST)
	var mask: PackedInt32Array = w.hooks().debug_island_tile_mask(eye, Vector3(0, 0, -1), TAN_X,
		TAN_Y, VIEW_W, VIEW_H)
	assert_int(mask.size()).is_equal(tiles_x * tiles_y)

	# Leftmost AABB corner: x = centre - half, z = centre - half, so
	# ndc = (OFFSET_X - HALF_SPAN) / ((DIST + HALF_SPAN) * TAN_X).
	var ndc_x := (OFFSET_X - HALF_SPAN) / ((DIST + HALF_SPAN) * TAN_X)
	var pixel_x := int((ndc_x + 1.0) * 0.5 * float(VIEW_W) - 0.5)
	var pixel_y := int((0.0 + 1.0) * 0.5 * float(VIEW_H) - 0.5)
	var tile_x := int(floor(float(pixel_x) / 16.0))
	var tile_y := int(floor(float(pixel_y) / 16.0))
	var probe_tile := tile_y * tiles_x + tile_x
	assert_int(tile_x).is_equal(7)
	assert_int(tile_y).is_equal(2)
	assert_int(mask[probe_tile] & 1).override_failure_message(
		"the tile containing the island's visible projection was not marked").is_not_equal(0)

func test_a_camera_just_outside_the_near_epsilon_still_marks_every_tile(timeout := 60000) -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_place_test_island(0, Vector3i(25, 62, 25), Vector3i(26, 63, 26),
		Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).is_true()
	var centre: Vector3 = d["world_center"]

	# Stand 5 mm outside the +z face of the (unrotated) 64-voxel lattice box and look almost
	# straight away from it, but tilted just enough that the far corner is a hair in front of
	# the eye plane: zmax ~0.0013, i.e. < 0.01 but >= 0. The near-clip fail-safe must mark
	# every tile; the old `zmax < 0.01` skip treated this close-but-not-behind box as fully
	# behind and dropped it.
	const HALF_SPAN := 1.575 # (kIslandDim - 1) * kIslandVoxelFine / 2
	const OUTSIDE := 0.005
	var eye := centre + Vector3(0.0, 0.0, HALF_SPAN + OUTSIDE)
	var dir := Vector3(0.004, 0.0, 1.0).normalized()
	var mask: PackedInt32Array = w.hooks().debug_island_tile_mask(eye, dir, TAN_X, TAN_Y, 128, 128)
	for m in mask:
		assert_int(m & 1).is_not_equal(0)

func test_teardown_physics_clears_stale_island_handoffs(timeout := 60000) -> void:
	var w := make_world()
	# The island atlas worker upload path is fixed at 64^3; use the real dim so this hook
	# still exercises the queue/teardown path rather than being rejected up front.
	const dim := 64
	var n := dim * dim * dim
	var sdf := PackedByteArray()
	sdf.resize(n)
	sdf.fill(0) # solid samples; only the queue state matters for this test
	var mat := PackedByteArray()
	mat.resize(n)
	mat.fill(0)
	w.hooks().debug_queue_test_island_upload(0, sdf, mat, dim)
	w.hooks().debug_queue_test_island_descriptors()
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"test island upload was not queued").is_equal(1)
	assert_int(w.hooks().debug_island_descriptors_pending()).override_failure_message(
		"test island descriptors were not marked dirty").is_equal(1)

	w.hooks().debug_teardown_physics()
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"teardown left stale island uploads queued").is_equal(0)
	assert_int(w.hooks().debug_island_descriptors_pending()).override_failure_message(
		"teardown left stale island descriptors dirty").is_equal(0)

	# Reinitializing physics must not drain those cleared entries into a fresh GPU pool.
	assert_bool(w.hooks().debug_init_physics()).is_true()
	assert_int(w.hooks().debug_island_pending_uploads()).is_equal(0)
	assert_int(w.hooks().debug_island_descriptors_pending()).is_equal(0)

func test_teardown_physics_preserves_committed_field_volume_uploads(timeout := 60000) -> void:
	var w := make_world()
	# Full 64^3 lattice: the worker's VolumePool is sized to kIslandDim, so a smaller dim
	# would be rejected by the actual worker upload even though the CPU queue can hold it.
	const dim := 64
	var n := dim * dim * dim
	var sdf := PackedByteArray()
	sdf.resize(n)
	sdf.fill(0) # solid samples; only the queue state matters for this test
	var mat := PackedByteArray()
	mat.resize(n)
	mat.fill(0)
	w.hooks().debug_queue_committed_field_volume_upload(0, sdf, mat, dim)
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"committed field-volume upload was not queued").is_equal(1)

	w.hooks().debug_teardown_physics()
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"teardown dropped a field-volume upload whose slot is pinned by a committed edit").is_equal(1)

	# Reinitializing physics must leave the committed upload queued so the new GPU pool can
	# receive the bytes before any op that names the pinned slot is evaluated.
	assert_bool(w.hooks().debug_init_physics()).is_true()
	assert_int(w.hooks().debug_island_pending_uploads()).override_failure_message(
		"committed field-volume upload was not preserved across physics re-init").is_equal(1)

	# The fresh MeshService must also have received the pinned volume itself, not just a
	# render-device queue entry; otherwise the worker's field evaluation reads a missing slot.
	var worker_slots: PackedInt32Array = w.hooks().debug_mesh_volume_slots()
	assert_bool(worker_slots.has(0)).override_failure_message(
		"new MeshService did not receive the pinned-volume replay: %s" % worker_slots
		).is_true()

# Regression for the freed-island magenta artifact. An island's material lattice only ever
# held a material where the field was SOLID, but every consumer reads it with nearest-sample
# rounding (island_material_at, sample_field_volume), so a hit point resolves to a sample on
# the AIR side of the surface about half the time and shaded as material_albedo(0) = error
# magenta. Extraction now projects near-surface air samples onto the surface, exactly as
# ve::spread_materials does for a brick.
#
# The component has to STRADDLE the terrain surface: a fully underground lump is bounded
# only by its own cell-box faces, and plan_island_lattice centres those on half-lattice
# coordinates where the nearest-sample rounding happens to land inside every time. The cell
# row is found by raycast rather than hard-coded, because the hills decide where it is.
#
# A fan of probes rather than one ray: the artifact is per-sample, so a single ray can miss
# it by luck while half the surface is magenta.
func is_magenta(c: Color) -> bool:
	# material_albedo(0) is (1, 0, 1) scaled by a lambert term in [0.25, 1]: green is exactly
	# zero. No real material shades below rock's 0.42 * 0.25 = 0.105.
	return c.g < 0.05 and c.r > 0.2

func test_no_ray_across_a_freed_island_resolves_to_error_magenta(timeout := 90000) -> void:
	var w := make_world()
	var ground: Dictionary = w.hooks().debug_raycast(Vector3(20.4, 90.0, 20.4), Vector3(0, -1, 0))
	assert_bool(ground["hit"]).override_failure_message(
		"could not find the terrain surface to cut the island out of").is_true()
	var top := int(floor((ground["pos"] as Vector3).y / 0.8))
	# Two cell rows spanning the surface, so the island's own top face IS terrain.
	var lo := Vector3i(25, top - 1, 25)
	var hi := Vector3i(26, top, 26)

	var d: Dictionary = w.hooks().debug_place_test_island(0, lo, hi, Vector3(0.0, 30.0, 0.0))
	assert_bool(d.get("ok", false)).override_failure_message(str(d)).is_true()
	assert_int(d["solid"]).override_failure_message(
		"the component came out empty, so there is no surface to shade").is_greater(0)
	var centre: Vector3 = d["world_center"]

	var hits := 0
	var magenta := 0
	for iz in range(10):
		for ix in range(10):
			var off := Vector3((ix - 4.5) * 0.16, 6.0, (iz - 4.5) * 0.16)
			var probe: Dictionary = w.hooks().debug_raymarch_probe(centre + off, Vector3(0, -1, 0))
			if not probe["hit"]:
				continue
			if (probe["pos"] as Vector3).y < centre.y - 3.0:
				continue # went past the island and hit the terrain 30 m below
			hits += 1
			if is_magenta(probe["color"]):
				magenta += 1
	assert_int(hits).override_failure_message(
		"no ray in the fan hit the island at all").is_greater(20)
	assert_int(magenta).override_failure_message(
		"%d of %d island hits shaded as error magenta (material 0)" % [magenta, hits]
		).is_equal(0)

func test_shared_storage_bounds_hold_for_an_island_render_world() -> void:
	# Task 6: islands render from the shared authoritative volume buffers, and the
	# compact-normal pool is a fixed 32 MiB (payload + both offset tables) that never
	# grows. This suite's world shrinks the atlas to 32x16x32 bricks, so the pinned R8
	# byte count here is 544 x 272 x 544; the DEFAULT 1088 x 544 x 544 count is asserted
	# by tests/test_stored_normal_pool.gd.
	var w := make_world()
	var stats: Dictionary = w.hooks().debug_stored_normal_stats()
	assert_int(stats["capacity_bytes"]).is_equal(33554432)
	assert_int(stats["live_bytes"]).is_less_equal(33554432)
	assert_int(stats["high_water_bytes"]).is_less_equal(33554432)
	var atlas: Dictionary = w.hooks().debug_atlas_stats()
	assert_int(atlas["sdf_atlas_bytes"]).is_equal(544 * 272 * 544)
