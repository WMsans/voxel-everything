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
	var V := _find_valley()
	var cam := Vector3(V.x, W + 3.0, V.z)
	_settle(_wa, cam)
	_settle(_wb, cam)
	var region := Vector3i(0, 0, 0)
	var found_region := false
	for ry in range(-2, 3):
		for rx in range(-2, 3):
			for rz in range(-2, 3):
				var cand := Vector3i(rx, ry, rz)
				if _wa.hooks().debug_region_map_entry(cand) >= 0 and _wb.hooks().debug_region_map_entry(cand) >= 0:
					region = cand
					found_region = true
					break
			if found_region:
				break
		if found_region:
			break
	var slot_id: int = _wa.hooks().debug_region_map_entry(region)
	var slot_id_b: int = _wb.hooks().debug_region_map_entry(region)
	var brick := region * 32 + Vector3i(4, 4, 4)
	var found_brick := false
	for by in range(32):
		for bx in range(32):
			for bz in range(32):
				var cand := region * 32 + Vector3i(bx, by, bz)
				var dd: Dictionary = _wa.hooks().debug_brick_diff(cand, slot_id, PackedByteArray(), 0)
				if int(dd.get("slot", -1)) >= 0:
					brick = cand
					found_brick = true
					break
			if found_brick:
				break
		if found_brick:
			break
	# (a) water world GPU vs CPU mirror: zero mismatches
	var d: Dictionary = _wa.hooks().debug_brick_diff(brick, slot_id, PackedByteArray(), 0)
	assert_int(d["slot"]).is_greater_equal(0)
	assert_int(d["sdf_max_diff"]).is_less_equal(1)
	assert_int(d["mat_near_mismatch"]).is_equal(0)
	# (b) per-brick opaque bytes equal across worlds
	var d_b: Dictionary = _wb.hooks().debug_brick_diff(brick, slot_id_b, PackedByteArray(), 0)
	var slot_a: int = int(d["slot"])
	var slot_b: int = int(d_b["slot"])
	assert_int(slot_b).is_greater_equal(0)
	var rd_a: RenderingDevice = _wa.hooks().debug_local_rd() as RenderingDevice
	var rd_b: RenderingDevice = _wb.hooks().debug_local_rd() as RenderingDevice
	var bytes_a: PackedByteArray = rd_a.texture_get_data(_wa.hooks().debug_sdf_atlas(), 0)
	var bytes_b: PackedByteArray = rd_b.texture_get_data(_wb.hooks().debug_sdf_atlas(), 0)
	var ax := 64
	var ay := 32
	var az := 32
	var stride := 17
	var atlas_x := ax * stride
	var atlas_y := ay * stride
	var extract := func(bytes: PackedByteArray, slot: int) -> PackedByteArray:
		var out := PackedByteArray()
		out.resize(stride * stride * stride)
		var sx := (slot % ax) * stride
		var sy := ((slot / ax) % ay) * stride
		var sz := (slot / (ax * ay)) * stride
		var idx := 0
		for z in range(stride):
			for y in range(stride):
				for x in range(stride):
					var off := ((sz + z) * atlas_y + (sy + y)) * atlas_x + (sx + x)
					out[idx] = bytes[off]
					idx += 1
		return out
	var block_a: PackedByteArray = extract.call(bytes_a, slot_a)
	var block_b: PackedByteArray = extract.call(bytes_b, slot_b)
	var first_diff := -1
	for i in range(block_a.size()):
		if block_a[i] != block_b[i]:
			first_diff = i
			break
	if first_diff >= 0:
		var msg := "first diff at byte %d (a=%d b=%d)" % [first_diff, block_a[first_diff], block_b[first_diff]]
		assert_bool(false).override_failure_message(msg).is_true()
	assert_bool(block_a == block_b).is_true()
