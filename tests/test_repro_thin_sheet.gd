extends GdUnitTestSuite

# Regression coverage for a sheet thinner than the 27-sample activation spacing. The
# streaming/occupancy path must generate the touched bricks from the edit range so the exact
# 5 cm lattice, not a missed activation sample, describes the resulting cells.

const CENTER := Vector3(20.0, 56.0, 20.0)

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(max_jobs := 16384, stream_radius := 40.0) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.max_brick_jobs = max_jobs
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.residency_radius_m = stream_radius
	w.atlas_bricks = Vector3i(48, 24, 48)
	w.max_region_slots = 64
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func make_tall_world(max_jobs := 16384, stream_radius := 40.0) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.max_brick_jobs = max_jobs
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 7, 8)
	w.residency_radius_m = stream_radius
	w.atlas_bricks = Vector3i(48, 24, 48)
	w.max_region_slots = 64
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	assert_bool(w.debug_init_physics()).is_true()
	return w

func tool_of(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

func step(w: VoxelWorld, frames: int, center: Vector3 = CENTER) -> void:
	for i in range(frames):
		w.debug_stream_frame(center)

# A shell thinner than the probe spacing: add a ball, subtract a slightly smaller ball at the
# same centre. What survives is a spherical skin `radius - inner` thick.
func test_edit_overflow_repair_preserves_thin_sheet_occupancy(timeout := 300000) -> void:
	var w := make_world(4096, 5.0)
	var t := tool_of(w)
	var sheet := Vector3(20.2, 62.2, 20.2)
	# First generate a 10 cm shell whose exact edit range is exactly eight bricks. Its
	# occupancy is solid even though every 27-sample probe lands more than the activation
	# pad away from the shell (a true probe miss, not just a probe sample outside the skin).
	step(w, 30)
	t.apply_sphere_add(sheet, 0.19, 4)
	step(w, 12)
	t.apply_sphere_subtract(sheet, 0.09)
	step(w, 12)
	var cell := Vector3i(25, 77, 25)
	assert_int(w.debug_cell_state(cell)).is_equal(2)
	assert_int(w.debug_occupancy_state(cell)).is_equal(2)
	# A separate paint edit in the same region expands the exact edit job list beyond 4096.
	# It changes no SDF, so the sheet remains a probe-missed thin surface while overflow
	# triggers the region recovery/repair paths.
	t.apply_sphere_paint(Vector3(23.0, 62.0, 23.0), 8.0, 4)
	# The overflow word is read back asynchronously, so poll a few frames for the sticky
	# job-list overflow bit. The single resident region keeps the re-mark from also forcing
	# eviction/reload, so this exercises the overflow/repair re-mark path itself.
	var overflow_seen := 0
	for i in range(5):
		w.debug_stream_frame(sheet)
		overflow_seen |= int(w.debug_stream_stats()["overflow_ever"])
	assert_int(overflow_seen & 2).override_failure_message(
		"the expanded exact-edit list did not overflow").is_not_equal(0)
	for i in range(90):
		w.debug_stream_frame(sheet)
	assert_int(w.debug_occupancy_state(cell)).override_failure_message(
		"overflow/repair recovery reverted the thin sheet to probe-only occupancy").is_equal(2)

func test_bounded_exact_edit_recovery_does_not_requeue_whole_region(timeout := 300000) -> void:
	# 4096 is above the single resident region's ordinary surface-brick count, so the
	# full-region PLAIN force-regen mark in recovery fits. It is far below the 32768-brick
	# whole-region exact-edit worst case, so an over-broad exact re-mark would overflow
	# again on every recovery frame and this test would never see the overflow bit clear.
	var w := make_tall_world(4096, 5.0)
	var t := tool_of(w)
	var c := Vector3(20.2, 90.0, 20.2)
	step(w, 30)
	# A chain of overlapping exact-edit ops in one high-air region, staying under the
	# 256-op region log. The normal edit path queues each op's own padded brick AABB, so
	# the same small set of bricks is queued many times and overflows the 4096 job list;
	# their union AABB stays far below 4096. That is exactly the overflow case a
	# whole-region exact re-mark would make unrecoverable (32768 > 4096), while the
	# bounded AABB re-mark fits.
	for i in range(80):
		t.apply_sphere_add(c + Vector3(0.0, i * 0.05, 0.0), 1.5, 4)
	w.debug_stream_frame(c)
	var overflow_seen := 0
	for i in range(5):
		w.debug_stream_frame(c)
		overflow_seen |= int(w.debug_stream_stats()["overflow_ever"])
	assert_int(overflow_seen & 2).override_failure_message(
		"the tiny exact-edit job list did not overflow").is_not_equal(0)
	var cleared := false
	for i in range(90):
		w.debug_stream_frame(c)
		if (int(w.debug_atlas_stats()["overflow"]) & 2) == 0:
			cleared = true
			break
	assert_bool(cleared).override_failure_message(
		"overflow recovery kept re-queueing the whole region instead of the bounded exact-edit AABB"
		).is_true()

func test_a_sheet_thinner_than_the_probe_spacing_is_generated(timeout := 300000) -> void:
	var w := make_world()
	var t := tool_of(w)
	# Well clear of the terrain so nothing else is in these cells.
	var c := Vector3(20.4, 62.0, 20.4)
	t.apply_sphere_add(c, 2.0, 4)
	step(w, 90)
	t.apply_sphere_subtract(c, 1.9) # a 10 cm skin
	step(w, 180)

	# The skin is unambiguously there: probe the field on the shell itself.
	var on_shell := 0
	for i in range(64):
		var a := TAU * float(i) / 64.0
		var p := c + Vector3(cos(a), 0.0, sin(a)) * 1.95
		if w.debug_field_sdf(p) < 0.0:
			on_shell += 1
	prints("field samples inside the 10 cm skin:", on_shell, "/ 64")
	assert_int(on_shell).override_failure_message("the skin was never built").is_greater(0)

	# The exact occupancy grid must retain the lattice cells containing the skin.
	var air := 0
	var solid := 0
	var cells := 0
	var lo := Vector3i(int(floor((c.x - 2.4) / 0.8)), int(floor((c.y - 2.4) / 0.8)),
		int(floor((c.z - 2.4) / 0.8)))
	var hi := Vector3i(int(floor((c.x + 2.4) / 0.8)), int(floor((c.y + 2.4) / 0.8)),
		int(floor((c.z + 2.4) / 0.8)))
	for cx in range(lo.x, hi.x + 1):
		for cy in range(lo.y, hi.y + 1):
			for cz in range(lo.z, hi.z + 1):
				var cell := Vector3i(cx, cy, cz)
				# Does the cell hold matter, on a 5 cm lattice inset off the faces?
				var holds := false
				for ix in range(16):
					for iy in range(16):
						for iz in range(16):
							var p := Vector3((cx * 16 + ix + 0.5) * 0.05,
								(cy * 16 + iy + 0.5) * 0.05, (cz * 16 + iz + 0.5) * 0.05)
							if w.debug_field_sdf(p) < 0.0:
								holds = true
								break
						if holds:
							break
					if holds:
						break
				if not holds:
					continue
				cells += 1
				if w.debug_cell_state(cell) == 1:
					air += 1
				else:
					solid += 1
	prints("cells holding matter:", cells, " reported AIR:", air,
		" reported solid/full:", solid)
	assert_int(cells).is_greater(0)
	assert_int(solid).override_failure_message(
		"the exact lattice did not retain any cell containing the 10 cm skin").is_greater(0)
	assert_int(air).is_equal(0)
