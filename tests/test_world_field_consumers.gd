extends GdUnitTestSuite
# Characterization for sub-project 5a (docs/superpowers/plans/2026-09-16-world-field-query.md,
# Task 5): what every consumer that moves onto ve::WorldField produces today. A refactor task
# must leave every golden below unchanged. Goldens are recorded, not derived: run once, paste
# each printed "<NAME> <json>" line's JSON into the matching const.
#
# Pinned elsewhere (plan decision 9): the island op-cap refusal (test_connectivity.gd), the
# merge-ground gate that reads raycast_down (test_island_body.gd, test_connectivity.gd), and
# the consolidation bake inputs (test_consolidation.gd).

const COLLIDER_GOLDEN := {"fill": [false, false, true, false, false, false, false, false, false, true, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false], "volume": [false, false, true, false, false, false, false, false, false, true, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false]}
const CONTACT_GOLDEN := {"counts": [81, 81, 81, 81, 65, 74, 81, 81, 81, 41, 60, 41, 41, 22, 0, 81, 60, 41, 41, 81, 41, 41, 65, 0, 81, 81, 41, 74, 65, 81, 74, 0, 74, 81, 65, 81, 0, 22, 41, 0, 0, 0, 81, 22, 41, 0, 65, 41, 0, 0, 0, 81, 65, 41, 81, 81, 81, 81, 65, 81, 81, 81, 81, 41, 60, 81, 41, 22, 81, 81, 60, 81, 41, 81, 81, 41, 65, 81, 81, 81, 81]}
const EXTRACT_GOLDEN := {"border": [true, 1, 0, 9802, 9802, 1], "inside": [true, 1, 0, 5740, 5740, 1]}
const RAY_GOLDEN := {"rays": [[true, [24.4, 49.399, 24.4], [0.0, 1.0, 0.0], 20.601, 3], [true, [27.0, 50.667, 24.4], [-0.481, 0.876, -0.011], 19.333, 3], [true, [12.8, 51.219, 12.8], [-0.093, 0.871, 0.483], 18.781, 3], [true, [6.4, 49.181, 20.0], [0.314, 0.946, 0.075], 20.819, 3], [true, [30.0, 46.339, 30.0], [0.0, 1.0, 0.0], 23.661, 1], [true, [40.8, 47.462, 40.8], [0.168, 0.822, 0.544], 32.538, 3], [true, [25.957, 50.145, 23.971], [-0.421, 0.906, 0.047], 21.106, 3], [true, [26.449, 51.42, 17.869], [-0.15, 0.955, 0.255], 26.444, 3], [false]]}

const PHYSICS_CENTER := Vector3(60.0, 55.0, 60.0)
const FILL_AT := Vector3(60.8, 73.6, 60.8)
const FILL_CHUNK := Vector3i(9, 11, 9)
const FILL_REGION := Vector3i(2, 2, 2)
const VDIM := 64
const VVOXEL := 0.05
const VRADIUS := 0.35
const VMATERIAL := 2
const VSLOT := 7

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func own(w: VoxelWorld) -> VoxelWorld:
	add_child(w)
	_worlds.append(w)
	return w

func make_physics_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.physics_radius_m = 25.0
	w.max_collider_chunks = 512
	w.mesh_jobs_per_frame = 2
	w.shape_builds_per_frame = 4
	own(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func make_golden_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.terrain_pipeline_path = "res://assets/pipelines/golden.pipeline"
	own(w)
	w.ensure_initialized()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

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

func store_ball(w: VoxelWorld, origin: Vector3) -> void:
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
	w.hooks().debug_store_volume(VSLOT, sdf, mat, VDIM)
	w.hooks().debug_apply_volume_add(VSLOT, origin, VVOXEL, VDIM)

func v3(v: Vector3) -> Array:
	return [snappedf(v.x, 0.001), snappedf(v.y, 0.001), snappedf(v.z, 0.001)]

func check(name: String, measured: Dictionary, golden: Dictionary) -> void:
	print(name, " ", JSON.stringify(measured))
	assert_bool(golden.is_empty()).override_failure_message(
		"no golden recorded: paste the %s line above into the const" % name).is_false()
	assert_str(JSON.stringify(measured)).override_failure_message(
		"%s moved" % name).is_equal(JSON.stringify(golden))

func collider_slots(w: VoxelWorld) -> Array:
	var out := []
	for z in range(FILL_CHUNK.z - 1, FILL_CHUNK.z + 2):
		for y in range(FILL_CHUNK.y - 1, FILL_CHUNK.y + 2):
			for x in range(FILL_CHUNK.x - 1, FILL_CHUNK.x + 2):
				out.append(int(w.hooks().debug_chunk_collider_info(Vector3i(x, y, z)).get("slot", -1)) >= 0)
	return out

# ColliderStreamer's residency probe (LogProbe): which chunks around a consolidated fill and
# around a pasted volume get a collider.
func test_collider_residency_is_pinned(timeout := 600000) -> void:
	var measured := {}
	var fill := make_physics_world()
	fill.hooks().debug_stream_region(FILL_REGION)
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	fill.add_child(tool)
	tool.apply_sphere_add(FILL_AT, 2.0, 4)
	assert_bool(fill.hooks().debug_consolidate_region(FILL_REGION)).is_true()
	assert_bool(settle(fill, PHYSICS_CENTER)).is_true()
	measured["fill"] = collider_slots(fill)
	var pasted := make_physics_world()
	store_ball(pasted, Vector3(59.225, 72.025, 59.225))
	assert_bool(settle(pasted, PHYSICS_CENTER)).is_true()
	measured["volume"] = collider_slots(pasted)
	check("COLLIDER_GOLDEN", measured, COLLIDER_GOLDEN)

# IslandManager's contact refinement probe (LogContactProbe): 27 cells x 3 axes around a
# consolidated carve.
func test_contact_samples_are_pinned(timeout := 300000) -> void:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	own(w)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_stream_region(Vector3i(0, 0, 0))
	w.hooks().debug_apply_sphere_subtract(Vector3(8.4, 16.8, 16.4), 1.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 0, 0))).is_true()
	var counts := []
	for z in range(19, 22):
		for y in range(19, 22):
			for x in range(9, 12):
				for axis in range(3):
					counts.append(w.hooks().debug_contact_samples(Vector3i(x, y, z), axis))
	check("CONTACT_GOLDEN", {"counts": counts}, CONTACT_GOLDEN)

# Island extract job inputs and the CPU reference: a lattice inside a consolidated region and
# one straddling its border.
func test_island_extract_is_pinned(timeout := 300000) -> void:
	var w := make_golden_world()
	w.hooks().debug_stream_region(Vector3i(0, 1, 0))
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 38.8, 16.4), 1.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 1, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(25.2, 39.6, 16.4), 0.5)
	var measured := {}
	for key in ["inside", "border"]:
		var lo := Vector3i(29, 48, 20) if key == "inside" else Vector3i(31, 48, 20)
		var hi := Vector3i(30, 49, 20) if key == "inside" else Vector3i(32, 49, 20)
		var d: Dictionary = w.hooks().debug_island_extract_diff(lo, hi)
		measured[key] = [bool(d.get("ok", false)), int(d.get("worst_steps", -1)),
				int(d.get("mat_mismatch", -1)), int(d.get("gpu_solid", -1)),
				int(d.get("cpu_solid", -1)), int(d.get("boxes", -1))]
	check("EXTRACT_GOLDEN", measured, EXTRACT_GOLDEN)

# The CPU raycast behind debug_raycast (and so edit_tool/hud): a fan over plain terrain, an
# unconsolidated carve, a consolidated carve, the analytic cave, a pasted volume and a miss.
func test_raycasts_are_pinned(timeout := 300000) -> void:
	var w := make_golden_world()
	w.hooks().debug_stream_region(Vector3i(0, 2, 0))
	w.hooks().debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
	assert_bool(w.hooks().debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
	w.hooks().debug_apply_sphere_subtract(Vector3(12.8, 55.2, 12.8), 3.0)
	store_ball(w, Vector3(39.225, 64.425, 39.225))
	var rays := [
		[Vector3(24.4, 70.0, 24.4), Vector3(0, -1, 0)],
		[Vector3(27.0, 70.0, 24.4), Vector3(0, -1, 0)],
		[Vector3(12.8, 70.0, 12.8), Vector3(0, -1, 0)],
		[Vector3(6.4, 70.0, 20.0), Vector3(0, -1, 0)],
		[Vector3(30.0, 70.0, 30.0), Vector3(0, -1, 0)],
		[Vector3(40.8, 80.0, 40.8), Vector3(0, -1, 0)],
		[Vector3(20.0, 70.0, 20.0), Vector3(0.3, -1.0, 0.2)],
		[Vector3(5.0, 60.0, 5.0), Vector3(1.0, -0.4, 0.6)],
		[Vector3(10.0, 80.0, 10.0), Vector3(0, 1, 0)],
	]
	var out := []
	for r in rays:
		var h: Dictionary = w.hooks().debug_raycast(r[0], r[1])
		if not h["hit"]:
			out.append([false])
			continue
		out.append([true, v3(h["pos"]), v3(h["normal"]), snappedf(float(h["distance"]), 0.001),
				int(h["material"])])
	check("RAY_GOLDEN", {"rays": out}, RAY_GOLDEN)
