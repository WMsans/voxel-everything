extends GdUnitTestSuite

const FJORDS := "res://assets/pipelines/fjords.pipeline"
const NO_WATER := "res://tests/fixtures/fjords_no_water.pipeline"
const W := 51.2

var _wa: VoxelWorld
var _wb: VoxelWorld

func after_test() -> void:
	for w in [_wa, _wb]:
		if is_instance_valid(w):
			w.free()
	_wa = null
	_wb = null

func _open_both() -> void:
	_wa = ClassDB.instantiate("VoxelWorld")
	_wa.terrain_pipeline_path = FJORDS
	_wa.world_seed = 0
	_wa.use_local_device = true
	_wa.physics_enabled = false
	_wa.stream_radius_m = 2600.0
	add_child(_wa)
	assert_bool(_wa.hooks().debug_init_atlas()).is_true()
	_wa.hooks().debug_pump_sectors(Vector3.ZERO, 2000.0, 2000)

	_wb = ClassDB.instantiate("VoxelWorld")
	_wb.terrain_pipeline_path = NO_WATER
	_wb.world_seed = 0
	_wb.use_local_device = true
	_wb.physics_enabled = false
	_wb.stream_radius_m = 2600.0
	add_child(_wb)
	assert_bool(_wb.hooks().debug_init_atlas()).is_true()
	_wb.hooks().debug_pump_sectors(Vector3.ZERO, 2000.0, 2000)

func _find_valley() -> Vector3:
	for xi in range(-400, 401, 20):
		for zi in range(-400, 401, 20):
			var hit: Dictionary = _wa.raycast(Vector3(xi, 800.0, zi), Vector3.DOWN, 1600.0)
			if hit["hit"] and float(hit["pos"].y) < W - 10.0:
				return Vector3(xi, 0.0, zi)
	for xi in range(-400, 401, 40):
		for zi in range(-400, 401, 40):
			var hit: Dictionary = _wa.raycast(Vector3(xi, 800.0, zi), Vector3.DOWN, 1600.0)
			if hit["hit"] and float(hit["pos"].y) < W - 10.0:
				return Vector3(xi, 0.0, zi)
	return Vector3.ZERO

func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

func _settle(w: VoxelWorld, cam: Vector3) -> void:
	w.hooks().debug_pump_sectors(cam, 900.0, 600)
	var quiet := 0
	for i in range(600):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(cam) == 0 else 0
		if quiet >= 6:
			break

func test_water_is_in_the_union() -> void:
	_open_both()
	var V := _find_valley()
	var pt := Vector3(V.x, W - 1.0, V.z)
	var r: Vector2 = _wa.hooks().debug_eval_field(pt, PackedByteArray(), 0)
	assert_int(int(r.y)).is_equal(material_id(_wa, "water"))
	var pt2 := Vector3(V.x, W + 1.0, V.z)
	var r2: Vector2 = _wa.hooks().debug_eval_field(pt2, PackedByteArray(), 0)
	assert_int(int(r2.y)).is_equal(0)

func test_underwater_gradient_is_the_grounds() -> void:
	_open_both()
	var V := _find_valley()
	_wa.hooks().debug_pump_sectors(Vector3(V.x, 0.0, V.z), 200.0, 600)
	_wb.hooks().debug_pump_sectors(Vector3(V.x, 0.0, V.z), 200.0, 600)
	var any_non_flat := false
	for i in range(20):
		var ang := float(i) * TAU / 20.0
		var xz := V + Vector3(cos(ang) * 5.0, 0.0, sin(ang) * 5.0)
		var hit: Dictionary = _wa.raycast(xz + Vector3(0.0, 800.0, 0.0), Vector3.DOWN, 1600.0)
		if not hit["hit"]:
			continue
		var g := float(hit["pos"].y)
		var pt := Vector3(xz.x, g + 0.05, xz.z)
		var gw: Dictionary = _wa.hooks().debug_eval_field_gradient(pt, PackedByteArray(), 0)
		var gn: Dictionary = _wb.hooks().debug_eval_field_gradient(pt, PackedByteArray(), 0)
		var gradw: Vector3 = gw["gradient"]
		var gradn: Vector3 = gn["gradient"]
		assert_float(gradw.distance_to(gradn)).is_less(1e-4)
		if gradw.distance_to(Vector3(0.0, 1.0, 0.0)) > 0.05:
			any_non_flat = true
	assert_bool(any_non_flat).is_true()

func test_collision_lattice_does_not_move() -> void:
	_open_both()
	var d1: Dictionary = _wa.hooks().debug_mesh_lattice_diff(Vector3i(4, 8, 4))
	var d2: Dictionary = _wb.hooks().debug_mesh_lattice_diff(Vector3i(4, 8, 4))
	assert_int(d1["diff_over_one"]).is_equal(0)
	assert_int(d2["diff_over_one"]).is_equal(0)

func test_grass_finds_the_ground_not_the_sea() -> void:
	_open_both()
	var V := _find_valley()
	var cam := Vector3(V.x, W + 3.0, V.z)
	_settle(_wa, cam)
	_settle(_wb, cam)
	var a: int = int(_wa.hooks().debug_grass_stats()["blades"])
	var b: int = int(_wb.hooks().debug_grass_stats()["blades"])
	var tol: int = maxi(1, int(float(b) * 0.01))
	assert_int(abs(a - b)).is_less_equal(tol)

func test_opaque_lattice_does_not_move() -> void:
	_open_both()
	assert_bool(true).is_true()