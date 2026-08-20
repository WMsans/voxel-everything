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

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(8, 5, 8)
	w.residency_radius_m = 40.0
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
