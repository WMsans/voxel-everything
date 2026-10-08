extends GdUnitTestSuite

# The sector tier on its fixture pipeline (plan Task 4): bake, read back, mirror. Every
# assertion reads what the shipping SectorContext and SectorCache produced.
const FIXTURE := "res://tests/fixtures/sector_fixture.pipeline"
const SECTOR_BYTES := 260 * 260 * 4

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func _open(seed := 0) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.terrain_pipeline_path = FIXTURE
	w.world_seed = seed
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func test_a_cold_world_bakes_the_sectors_around_the_camera() -> void:
	var w := _open()
	var frames: int = w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)
	assert_int(frames).is_greater(0)
	var s: Dictionary = w.hooks().debug_sector_stats()
	assert_bool(s["enabled"]).is_true()
	assert_int(s["resident"]).is_greater(0)
	assert_int(s["render_layers"]).is_equal(s["resident"])
	assert_int(s["over_limit"]).is_equal(0)
	assert_float(s["max_slope"]).is_greater(0.0).is_less(1.0)

func test_the_same_sector_bakes_to_identical_bytes() -> void:
	var w := _open()
	assert_int(w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	var a: PackedByteArray = w.hooks().debug_sector_texels(0, 0)
	assert_int(a.size()).is_equal(SECTOR_BYTES)
	w.hooks().debug_sector_clear()
	assert_int(w.hooks().debug_sector_texels(0, 0).size()).is_equal(0)
	assert_int(w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	assert_bool(w.hooks().debug_sector_texels(0, 0) == a).is_true()

# Review Focus 4: the title flow builds one world after another in one process.
func test_a_second_world_never_reads_the_first_worlds_texels() -> void:
	var a := _open(0)
	assert_int(a.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	var bytes_a: PackedByteArray = a.hooks().debug_sector_texels(0, 0)
	a.free()
	# A far-offset seed: Review Focus 1 (sector coordinates thousands of metres out).
	var b := _open(5)
	var off: Vector3 = b.field_offset()
	assert_float(maxf(absf(off.x), absf(off.z))).is_greater(1000.0)
	assert_int(b.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	var home := Vector2i(floori(off.x / 409.6), floori(off.z / 409.6))
	var bytes_b: PackedByteArray = b.hooks().debug_sector_texels(home.x, home.y)
	assert_int(bytes_b.size()).is_equal(SECTOR_BYTES)
	assert_bool(bytes_b == bytes_a).is_false()
	b.free()
	var c := _open(0)
	assert_int(c.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	assert_bool(c.hooks().debug_sector_texels(0, 0) == bytes_a).is_true()

# The mesher runs on its own RenderingDevice (plan deviation 1). Its lattice must match the
# CPU field over a fixture surface, which it can only do if its own mirror holds the sector.
func test_the_mesher_reads_the_same_sectors_as_the_cpu() -> void:
	var w := _open()
	assert_int(w.hooks().debug_pump_sectors(Vector3.ZERO, 300.0, 200)).is_greater(0)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var hit: Dictionary = w.raycast(Vector3(30.0, 600.0, 30.0), Vector3.DOWN, 1200.0)
	assert_bool(hit["hit"]).is_true()
	var chunk := Vector3i((hit["pos"] as Vector3 / 6.4).floor())
	var d: Dictionary = w.hooks().debug_mesh_lattice_diff(chunk)
	assert_bool(d["has_surface"]).is_true()
	assert_int(d["max_diff"]).is_less_equal(1)
	assert_int(d["diff_over_one"]).is_equal(0)
