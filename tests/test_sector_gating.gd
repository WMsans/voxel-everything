extends GdUnitTestSuite

# Spec §5.3: nothing that bakes field data into a store runs over a sector that is not
# resident. Fixture pipeline; every assertion reads the shipping streamer, LoD system and
# collider streamer.
const FIXTURE := "res://tests/fixtures/sector_fixture.pipeline"
const CAM := Vector3(10.0, 60.0, 10.0)

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func _open() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.terrain_pipeline_path = FIXTURE
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	return w

func _stream(w: VoxelWorld, cam: Vector3, frames: int) -> void:
	for i in range(frames):
		w.hooks().debug_stream_frame(cam)

func test_no_region_streams_over_an_unbaked_sector() -> void:
	var w := _open()
	_stream(w, CAM, 10)
	assert_int(w.hooks().debug_stream_stats()["resident_regions"]).is_equal(0)
	assert_int(w.hooks().debug_pump_sectors(CAM, 200.0, 300)).is_greater(0)
	_stream(w, CAM, 10)
	assert_int(w.hooks().debug_stream_stats()["resident_regions"]).is_greater(0)

# Review Focus 2.
func test_a_teleport_holds_regions_until_their_sectors_bake() -> void:
	var w := _open()
	assert_int(w.hooks().debug_pump_sectors(CAM, 200.0, 300)).is_greater(0)
	_stream(w, CAM, 10)
	var far := Vector3(3000.0, 60.0, 3000.0)
	var far_region := Vector3i((far / 25.6).floor())
	# The cache is still centred on the old camera: nothing at the new one is wanted yet.
	_stream(w, far, 10)
	assert_int(w.hooks().debug_slot_of_region(far_region)).is_equal(-1)
	assert_int(w.hooks().debug_pump_sectors(far, 200.0, 300)).is_greater(0)
	_stream(w, far, 20)
	assert_int(w.hooks().debug_slot_of_region(far_region)).is_greater_equal(0)

func test_lod_requests_wait_for_their_sectors() -> void:
	var w := _open()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_tick(CAM, Vector3(1.0, -0.2, 0.0))
	var s: Dictionary = w.hooks().debug_lod_stats()
	assert_int(s["sector_held"]).is_greater(0)
	assert_int(s["builds_in_flight"]).is_equal(0)
	assert_int(w.hooks().debug_pump_sectors(CAM, 1000.0, 600)).is_greater(0)
	w.hooks().debug_lod_tick(CAM, Vector3(1.0, -0.2, 0.0))
	assert_int(w.hooks().debug_lod_stats()["sector_held"]).is_less(s["sector_held"])

func test_colliders_wait_for_their_sectors() -> void:
	var w := _open()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	for i in range(5):
		w.hooks().debug_physics_frame(CAM)
	var s: Dictionary = w.hooks().debug_physics_stats()
	assert_int(s["sector_holds"]).is_greater(0)
	assert_int(s["probe_cache"]).is_equal(0) # nothing probed fallback air
	assert_int(w.hooks().debug_pump_sectors(CAM, 200.0, 300)).is_greater(0)
	for i in range(30):
		w.hooks().debug_physics_frame(CAM)
	assert_int(w.hooks().debug_physics_stats()["chunks_resident"]).is_greater(0)
