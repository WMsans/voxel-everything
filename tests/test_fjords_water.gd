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

# The terrain pipeline's field source is installed through a PROCESS-GLOBAL override
# (ve::set_shader_source_override), so the last world to initialise decides which field every
# pass compiled after it sees. Two worlds with DIFFERENT pipelines may therefore only be used
# for passes initialised before the second world exists (the render device, at
# debug_init_atlas). Anything on the mesh worker (debug_init_physics, the shell and collision
# lattice) must use ONE world at a time -- see test_shell_candidates_at_the_shore.
func _open_both() -> void:
	_wa = _make_world(FJORDS)
	_wb = _make_world(NO_WATER)

func _make_world(path: String, pump_at = Vector3.ZERO) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.terrain_pipeline_path = path
	w.world_seed = 0
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 2600.0
	add_child(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	if pump_at != null:
		w.hooks().debug_pump_sectors(pump_at, 2000.0, 2000)
	return w

func _find_valley(w: VoxelWorld = null) -> Vector3:
	if w == null:
		w = _wa
	for step in [20, 40]:
		for xi in range(-400, 401, step):
			for zi in range(-400, 401, step):
				var hit: Dictionary = w.raycast(Vector3(xi, 800.0, zi), Vector3.DOWN, 1600.0)
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

func test_removing_water_leaves_a_hole() -> void:
	# The sea is the base field, but ops carve the UNION, so a sphere subtract must open it and
	# the hole must stay: terrain water does not refill (water spec §2, non-goals). The edit
	# raycast marches the union with the store's ops, which debug_eval_field cannot (it is
	# handed an explicit op list), so the ray is the honest probe.
	_wa = _make_world(FJORDS)
	var cam := Vector3(0.0, W + 3.0, 0.0) # the origin is over deep water: ground ~64 m down
	var eye := Vector3(cam.x, W + 60.0, cam.z)
	var before: Dictionary = _wa.raycast(eye, Vector3.DOWN, 200.0)
	assert_bool(before["hit"]).is_true()
	assert_float(absf(float(before["pos"].y) - W)).override_failure_message(
			"no sea surface at the origin: %s" % before).is_less(0.1)
	# r = 2 centred 1 m over the surface: the sphere's lower cap is 1 m BELOW it, so a
	# downward ray must end there instead of on the surface.
	_wa.hooks().debug_apply_sphere_subtract(Vector3(cam.x, W + 1.0, cam.z), 2.0)
	_settle(_wa, cam)
	var after: Dictionary = _wa.raycast(eye, Vector3.DOWN, 200.0)
	assert_bool(after["hit"]).is_true()
	assert_float(float(after["pos"].y)).override_failure_message(
			"the sea surface did not open: %s" % after).is_less(W - 0.5)
	for i in range(120):
		_wa.hooks().debug_stream_frame(cam)
	var later: Dictionary = _wa.raycast(eye, Vector3.DOWN, 200.0)
	assert_bool(later["hit"]).is_true()
	assert_float(float(later["pos"].y)).override_failure_message(
			"the hole refilled: %s" % later).is_less(W - 0.5)

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
	# ONE world at a time (process-global field source): the mesh worker's lattice must be
	# sampled from the field its own world installed, or the comparison is dry-vs-dry.
	# The no-water world goes first: its raycast reaches the GROUND, while the water world's
	# stops at the sea surface (y = W, on a chunk boundary) and would pick an all-air chunk.
	_wb = _make_world(NO_WATER)
	var V := _find_valley(_wb)
	var hit: Dictionary = _wb.raycast(Vector3(V.x, 800.0, V.z), Vector3.DOWN, 1600.0)
	assert_bool(hit["hit"]).is_true()
	# The chunk that holds the sea floor at V, so the lattice really crosses a surface. A chunk
	# of pure air would match its CPU mirror trivially and prove nothing.
	var chunk := Vector3i(floori(V.x / 6.4), floori(float(hit["pos"].y) / 6.4), floori(V.z / 6.4))
	assert_bool(_wb.hooks().debug_init_physics()).is_true()
	var d2: Dictionary = _wb.hooks().debug_mesh_lattice_diff(chunk)
	_wb.free()
	_wb = null

	_wa = _make_world(FJORDS)
	assert_bool(_wa.hooks().debug_init_physics()).is_true()
	var d1: Dictionary = _wa.hooks().debug_mesh_lattice_diff(chunk)
	assert_int(d1["diff_over_one"]).override_failure_message("water world: %s" % d1).is_equal(0)
	assert_int(d2["diff_over_one"]).override_failure_message("no-water world: %s" % d2).is_equal(0)
	assert_bool(d1["has_surface"]).override_failure_message("dry chunk: %s" % d1).is_true()
	assert_bool(d2["has_surface"]).override_failure_message("dry chunk: %s" % d2).is_true()
	assert_int(int(d1["max_diff"])).is_equal(int(d2["max_diff"]))

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

func _shell_pages_after(w: VoxelWorld, cam: Vector3, frames: int) -> Dictionary:
	var stats := {}
	for i in range(frames):
		w.hooks().debug_stream_frame(cam)
		w.hooks().debug_render_frame(cam, Vector3(0.0, -1.0, 0.0).normalized(), 64, 64)
		stats = w.hooks().debug_lod_stats()
		if int(stats.get("shell_pages", 0)) > 0:
			break
	return stats

func test_shell_candidates_at_the_shore() -> void:
	# ONE world at a time: the field source is process-global, so a no-water world alive next
	# to the water world would compile the water world's LOD worker against the dry field and
	# the shell would come out empty for the wrong reason.
	var finder := _make_world(FJORDS)
	var V := _find_valley(finder)
	finder.free()
	var cam := Vector3(V.x, W + 3.0, V.z)

	_wa = _make_world(FJORDS)
	assert_bool(_wa.hooks().debug_init_physics()).is_true()
	_wa.hooks().debug_pump_sectors(cam, 900.0, 2000)
	var stats := _shell_pages_after(_wa, cam, 600)
	assert_int(int(stats.get("shell_pages", 0))).override_failure_message("water world no shell: %s" % stats).is_greater(0)
	_wa.free()
	_wa = null

	_wb = _make_world(NO_WATER)
	assert_bool(_wb.hooks().debug_init_physics()).is_true()
	_wb.hooks().debug_pump_sectors(cam, 900.0, 2000)
	var stats_b := _shell_pages_after(_wb, cam, 120)
	assert_int(int(stats_b.get("shell_pages", 0))).override_failure_message("no-water world has a shell: %s" % stats_b).is_equal(0)

func test_candidates_appear_when_sectors_land() -> void:
	# The valley point, found in a throwaway world, then a fresh one at it: no sector is
	# resident when the camera is set, so the candidates can only appear when sectors land.
	# Both worlds use the same pipeline, so the process-global field source stays Fjords.
	var finder := _make_world(FJORDS)
	var V := _find_valley(finder)
	finder.free()
	var cam := Vector3(V.x, W + 3.0, V.z)
	_wa = _make_world(FJORDS, null)
	var w := _wa
	assert_bool(w.hooks().debug_init_physics()).is_true()
	# Un-pump: this world starts the tick with nothing resident around the camera.
	assert_int(int(w.hooks().debug_lod_stats().get("shell_pages", 0))).is_equal(0)
	w.hooks().debug_pump_sectors(cam, 900.0, 2000)
	var stats := _shell_pages_after(w, cam, 600)
	assert_int(int(stats.get("shell_pages", 0))).override_failure_message("no shell after sectors: %s" % stats).is_greater(0)


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

func _tick(w: VoxelWorld, cam: Vector3, frames: int) -> Dictionary:
	for i in range(frames):
		w.hooks().debug_stream_frame(cam)
		w.hooks().debug_lod_tick(cam, Vector3(0.0, -1.0, 0.0))
	return w.hooks().debug_lod_stats()

func test_shell_candidates_follow_the_fade_band() -> void:
	# The candidate radius is fade_end + one shell chunk, and fade_end tracks the residency
	# reach, which moves while sectors stream in. A parked camera must not keep the set it
	# built under an earlier band: the ring between the old and new fade_end then has no near
	# shell, and the near half of the dither shows bare ground (the seam band). Stepping one
	# chunk away and back forces a recompute at today's band; a parked set must equal it.
	var finder := _make_world(FJORDS)
	var V := _find_valley(finder)
	finder.free()
	var cam := Vector3(V.x, W + 25.0, V.z)
	# Sectors first, so no sector landing recomputes the set once the camera is parked: the
	# only thing that moves after the first tick is the band itself.
	_wa = _make_world(FJORDS, cam)
	assert_bool(_wa.hooks().debug_init_physics()).is_true()
	var band0: Vector2 = _wa.hooks().debug_lod_fade_band()
	var parked: Dictionary = _tick(_wa, cam, 300)
	var band1: Vector2 = _wa.hooks().debug_lod_fade_band()
	_tick(_wa, cam + Vector3(3.2, 0.0, 0.0), 2)
	var forced: Dictionary = _tick(_wa, cam, 2)
	assert_int(int(parked["shell_chunks"])).override_failure_message(
			"parked %d vs recomputed %d candidates; band %s -> %s" % [parked["shell_chunks"],
			forced["shell_chunks"], band0, band1]).is_equal(int(forced["shell_chunks"]))

func test_a_shaft_dug_into_the_sea_is_a_shell_candidate() -> void:
	# The terrain water candidate is the plane y = W padded by one cell: two chunk layers. A
	# shaft dug below them exposes water walls that only a candidate chunk can mesh; without
	# one the near-field marcher (which sees water as air) shows the bare floor through the
	# hole, unshaded. The subtract must make the chunks it reaches under W candidates.
	var finder := _make_world(FJORDS)
	var V := _find_valley(finder)
	finder.free()
	var cam := Vector3(V.x, W + 6.0, V.z)
	_wa = _make_world(FJORDS, cam)
	assert_bool(_wa.hooks().debug_init_physics()).is_true()
	var before: Dictionary = _tick(_wa, cam, 300)
	for k in range(13):
		_wa.hooks().debug_apply_sphere_subtract(Vector3(V.x, W + 1.0 - k, V.z + 8.0), 2.5)
	var after: Dictionary = _tick(_wa, cam, 300)
	# The shaft reaches W - 14.5: four chunk layers under the plane's two, about four chunks
	# of 3.2 m across each.
	assert_int(int(after["shell_chunks"]) - int(before["shell_chunks"])).override_failure_message(
			"candidates %d -> %d" % [before["shell_chunks"], after["shell_chunks"]]).is_greater_equal(16)
