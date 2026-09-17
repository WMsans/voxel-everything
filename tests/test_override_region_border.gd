extends GdUnitTestSuite
# S3 (docs/superpowers/specs/2026-09-16-world-field-query-design.md §3). The GPU reads a baked
# override through ONE table per job and indexes it with `brick & 31`, so a sample outside
# that table's region reads a brick of the wrong region. The CPU OverrideStore resolves every
# brick globally and is the oracle.
#
# Fixture: the golden pipeline's analytic terrain. At y = 38.8 there is at least 2.4 m of rock
# everywhere (surface = 51.2 + hills, hills >= -10; the analytic cave spans y 44..54 around
# x = z = 30), so each carve below is a closed pocket and the CPU field beside it is clamped
# rock. Region R = (0, 1, 0) spans x 0..25.6, y 25.6..51.2, z 0..25.6.

const R := Vector3i(0, 1, 0)
const R_CENTER := Vector3(12.8, 38.4, 12.8)
const ROCK_Y := 38.8   # brick y 48
const ROCK_Z := 16.4   # brick z 20

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
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	add_child(w)
	_worlds.append(w)
	w.ensure_initialized()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_stream_region(R)
	assert_int(w.hooks().debug_slot_of_region(R)).is_greater_equal(0)
	return w

func assert_rock(w: VoxelWorld, points: Array) -> void:
	for p in points:
		assert_float(w.hooks().debug_field_sdf(p)).override_failure_message(
			"fixture: %s is not deep rock on this terrain" % p).is_less(-0.6)

func carve_and_consolidate(w: VoxelWorld, centre: Vector3, radius: float) -> void:
	w.hooks().debug_apply_sphere_subtract(centre, radius)
	assert_bool(w.hooks().debug_consolidate_region(R)).is_true()
	assert_int(w.hooks().debug_region_op_count(R)).is_equal(0)

func make_op(type: int, material: int, pos: Vector3, radius: float,
		aux0: int = 0, aux1: int = 0) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(aux0); b.put_u32(aux1)
	return b.data_array

# S3a, region-map shaders. Brick (31, 48, 20)'s +x apron plane is x = 25.6, brick 32 of the
# neighbour region; `32 & 31` is brick 0 of R, which holds the baked carve. The pocket makes
# brick 31 resident so the atlas holds it. This half may be float-boundary dependent (plan
# decision 11): green closes it with this test as evidence.
func test_a_brick_beside_a_region_border_reads_no_wrapped_override(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(0.4, ROCK_Y, ROCK_Z), Vector3(25.6, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(0.4, ROCK_Y, ROCK_Z), 0.6)
	var pocket := Vector3(25.0, ROCK_Y, ROCK_Z)
	w.hooks().debug_apply_sphere_subtract(pocket, 0.25)
	for i in range(8):
		w.hooks().debug_stream_frame(R_CENTER)
	var slot: int = w.hooks().debug_slot_of_region(R)
	var d: Dictionary = w.hooks().debug_brick_diff(Vector3i(31, 48, 20), slot,
			make_op(0, 0, pocket, 0.25), 1)
	assert_int(int(d.get("slot", -1))).override_failure_message(
		"fixture: brick (31,48,20) is not resident: %s" % d).is_greater_equal(0)
	assert_int(int(d["sdf_max_diff"])).override_failure_message(
		"S3a: the generated brick read R's brick-0 override through the & 31 wrap: %s" % d
		).is_less_equal(1)

# S3a, single-table job. Chunk (0, 6, 2) starts at x = 0; its lattice's first plane is
# x = -0.1, brick -1, and `-1 & 31` is brick 31 of the chunk's own region, 25.5 m away.
func test_a_collider_chunk_lattice_reads_no_wrapped_override(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(25.2, ROCK_Y, ROCK_Z), Vector3(-0.1, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(25.2, ROCK_Y, ROCK_Z), 0.6)
	var d: Dictionary = w.hooks().debug_mesh_diff(Vector3i(0, 6, 2))
	assert_bool(d.has("lattice_max_diff")).override_failure_message(
		"fixture: mesh diff did not run: %s" % d).is_true()
	assert_int(int(d["lattice_max_diff"])).override_failure_message(
		"S3a: the chunk lattice read brick 31's override through the & 31 wrap: %s" % d
		).is_less_equal(1)

# S3a, single-table job. The lattice for cells (31..32, 48, 20) starts in R and takes R's
# table; its samples at x >= 25.6 wrap onto R's bricks 0..1, which hold the baked carve.
func test_an_island_lattice_across_a_region_border_reads_no_wrapped_override(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(0.4, ROCK_Y, ROCK_Z), Vector3(26.0, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(0.4, ROCK_Y, ROCK_Z), 0.6)
	var d: Dictionary = w.hooks().debug_island_extract_diff(Vector3i(31, 48, 20), Vector3i(32, 48, 20))
	assert_bool(d.get("ok", false)).override_failure_message("fixture: extraction failed: %s" % d).is_true()
	assert_int(int(d["worst_steps"])).override_failure_message(
		"S3a: the island lattice read R's brick-0 override across the border: %s" % d).is_less(2)

# S3b. The level-1 chunk (1, 1, 0) has its origin in R' = (1, 1, 0) and takes R's (none)
# table, but its fine lattice's first samples (x = 24.4, 24.8) lie in R, over a baked carve
# centred at (24.0, 38.8, 16.4). This case purely guards table resolution (origin-region
# table vs sample-region table): the carve sits off the border (centre x = 24.0, radius
# 0.6, so with the 0.20 m append pad its max-x is 24.8, clear of x = 25.6), so the op
# appends R-only and no live cross-region op can reach the LoD job's chunk-wide gather.
func test_a_lod_chunk_reads_overrides_of_every_region_it_covers(timeout := 240000) -> void:
	var w := make_world()
	assert_rock(w, [Vector3(24.8, ROCK_Y, ROCK_Z)])
	carve_and_consolidate(w, Vector3(24.0, ROCK_Y, ROCK_Z), 0.6)
	var d: Dictionary = w.hooks().debug_lod_diff(1, Vector3i(1, 1, 0))
	assert_bool(d.has("fine_max_diff")).override_failure_message("fixture: LoD diff did not run: %s" % d).is_true()
	assert_int(int(d["fine_max_diff"])).override_failure_message(
		"S3b: the LoD chunk ignored the override of a region other than its origin's: %s" % d
		).is_less_equal(1)
