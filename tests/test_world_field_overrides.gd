extends GdUnitTestSuite
# S1 and S2 (docs/superpowers/specs/2026-09-13-frame-module-design.md §10): CPU probes of the
# world field must read what consolidation baked, not only the region's op list. Consolidation
# bakes a region's ops into override bricks and clears the list, so a probe that ignores
# overrides sees the terrain as it was before the edit.

const PHYSICS_CENTER := Vector3(60.0, 55.0, 60.0)
# Open sky inside the physics ball. NOTE (Task 3 deviation): the brief placed the fill at
# y = 67.2 in chunk (9,10,9) assuming terrain tops out at 51.2 + 10 m, but the default
# terrain pipeline adds the relief stage (~+9 m at this location), so the generator-only
# probe already returns true there (min lattice sdf 2.18 < pad 2.77) and the test passed
# pre-fix. Moved up one chunk: generator-only min sdf is 8.58 vs pad 2.77 here.
const FILL_AT := Vector3(60.8, 73.6, 60.8)
const FILL_CHUNK := Vector3i(9, 11, 9)   # floor(FILL_AT / 6.4)
const FILL_REGION := Vector3i(2, 2, 2)   # floor(FILL_AT / 25.6)

# A 64^3 volume at 5 cm holding a solid ball (r = 0.35), centred on the chunk probe's
# 1.6 m lattice point (60.8, 73.6, 60.8): chunk (9,11,9) starts at (57.6, 70.4, 57.6),
# so the lattice point is origin + (3.2, 3.2, 3.2) and VORIGIN is that minus 1.575.
# NOTE (Task 3 deviation): the brief used dim 16, but the mesher worker's volume pool is
# fixed at 64^3 (cf. test_consolidation.gd "dim 64 to match worker pool"), so a dim-16
# volume never uploads: the fixed CPU probe then submits a build the worker meshes empty
# and the slot is freed again. Production volumes are always 64^3 (kIslandDim).
const VDIM := 64
const VVOXEL := 0.05
const VORIGIN := Vector3(59.225, 72.025, 59.225)
const VRADIUS := 0.35
const VMATERIAL := 2
const VSLOT := 7

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_physics_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# Same contract as tests/test_collider_edits.gd: the streamer owes nothing.
func settle(w: VoxelWorld, center: Vector3, frames := 6000) -> bool:
	var quiet := 0
	for i in range(frames):
		w.hooks().debug_physics_frame(center)
		var st := w.hooks().debug_physics_stats()
		quiet = quiet + 1 if st["chunks_pending"] == 0 and st["queued"] == 0 else 0
		if quiet >= 4:
			return true
		OS.delay_msec(1)
	return false

func encode_sdf(d: float) -> int:
	var t := clampf((d + 0.64) / 1.28, 0.0, 1.0)
	return int(floor(t * 255.0 + 0.5))

func ball_volume() -> Array:
	var sdf := PackedByteArray()
	var mat := PackedByteArray()
	sdf.resize(VDIM * VDIM * VDIM)
	mat.resize(VDIM * VDIM * VDIM)
	var c := 0.5 * float(VDIM - 1) * VVOXEL
	for z in range(VDIM):
		for y in range(VDIM):
			for x in range(VDIM):
				var d := (Vector3(x, y, z) * VVOXEL - Vector3(c, c, c)).length() - VRADIUS
				var i := x + y * VDIM + z * VDIM * VDIM
				sdf[i] = encode_sdf(d)
				mat[i] = VMATERIAL if d <= 0.0 else 0
	return [sdf, mat]

# S1, overrides. ChunkResidency never re-probes a chunk that is already resident, so the chunk
# must be probed for the first time AFTER the bake: the physics streamer has not run yet here.
func test_a_consolidated_fill_in_open_sky_still_gets_a_collider(timeout := 180000) -> void:
	var w := make_physics_world()
	w.hooks().debug_stream_region(FILL_REGION)
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	var r: Dictionary = tool.apply_sphere_add(FILL_AT, 2.0, 4)
	assert_array(r["rejected"]).is_empty()
	assert_bool(w.hooks().debug_consolidate_region(FILL_REGION)).is_true()
	assert_int(w.hooks().debug_region_op_count(FILL_REGION)).is_equal(0)
	assert_bool(settle(w, PHYSICS_CENTER)).is_true()
	var info: Dictionary = w.hooks().debug_chunk_collider_info(FILL_CHUNK)
	assert_int(int(info.get("slot", -1))).override_failure_message(
		"the filled chunk probed as empty once its fill was baked: %s" % info).is_greater_equal(0)

# S1, volumes. A pasted volume is an op naming a volume slot; without the volume store the
# probe evaluates it as nothing.
func test_a_pasted_volume_in_open_sky_gets_a_collider(timeout := 180000) -> void:
	var w := make_physics_world()
	var ball := ball_volume()
	w.hooks().debug_store_volume(VSLOT, ball[0], ball[1], VDIM)
	w.hooks().debug_apply_volume_add(VSLOT, VORIGIN, VVOXEL, VDIM)
	assert_bool(settle(w, PHYSICS_CENTER)).is_true()
	var info: Dictionary = w.hooks().debug_chunk_collider_info(FILL_CHUNK)
	assert_int(int(info.get("slot", -1))).override_failure_message(
		"the chunk holding only a pasted volume probed as empty: %s" % info).is_greater_equal(0)

const CARVE_REGION := Vector3i(0, 0, 0)

func make_extract_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_stream_region(CARVE_REGION)
	return w

# S2, contact probe. The face between cells (10,20,20) and (10,21,20) is the plane y = 16.8 m,
# x in [8, 8.8], z in [16, 16.8]: solid rock, all 81 samples.
func test_the_contact_probe_reads_a_consolidated_carve(timeout := 120000) -> void:
	var w := make_extract_world()
	assert_int(w.hooks().debug_contact_samples(Vector3i(10, 20, 20), 1)).is_equal(81)
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	tool.apply_sphere_subtract(Vector3(8.4, 16.8, 16.4), 1.0)
	assert_int(w.hooks().debug_contact_samples(Vector3i(10, 20, 20), 1)).is_equal(0)
	assert_bool(w.hooks().debug_consolidate_region(CARVE_REGION)).is_true()
	assert_int(w.hooks().debug_region_op_count(CARVE_REGION)).is_equal(0)
	assert_int(w.hooks().debug_contact_samples(Vector3i(10, 20, 20), 1)).override_failure_message(
		"the contact probe sees uncarved rock once the carve is baked").is_equal(0)

# S2, CPU island extract. The GPU extraction samples a snapshot that includes overrides; the
# CPU reference must agree after the carve moved into them.
func test_the_cpu_island_extract_reads_a_consolidated_carve(timeout := 120000) -> void:
	var w := make_extract_world()
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	tool.apply_sphere_subtract(Vector3(8.4, 16.4, 16.4), 0.6)
	assert_bool(w.hooks().debug_consolidate_region(CARVE_REGION)).is_true()
	assert_int(w.hooks().debug_region_op_count(CARVE_REGION)).is_equal(0)
	var d: Dictionary = w.hooks().debug_island_extract_diff(Vector3i(10, 20, 20), Vector3i(11, 20, 20))
	assert_bool(d.get("ok", false)).override_failure_message("extraction failed: %s" % d).is_true()
	assert_int(d["worst_steps"]).override_failure_message(
		"CPU and GPU extraction disagree after consolidation: worst %d steps" % d["worst_steps"]
		).is_less(2)
	assert_int(d["mat_mismatch"]).is_equal(0)
