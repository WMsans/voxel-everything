extends GdUnitTestSuite

# DIAGNOSTIC ONLY. Hypothesis under test:
#
#   ve::cell_state_field (and shaders/brick_mark.comp.glsl, which computes the same thing)
#   decides a 0.8 m cell's state from a 3x3x3 lattice of SDF samples -- a 0.4 m spacing.
#   A cell reads kCellAir when ALL 27 samples are positive. Matter thinner than that spacing
#   can contain no sample point at all, so a thin sheet reads AIR.
#
#   Everything downstream believes it: ve::flood_anchored only walks `solid` cells, so the
#   sheet is never in any labelled component, is never carved, and is left standing as static
#   terrain after the island around it becomes a body.

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
		w.debug_physics_frame(center)
		w.debug_island_frame(1.0 / 60.0, center)

# A shell thinner than the probe spacing: add a ball, subtract a slightly smaller ball at the
# same centre. What survives is a spherical skin `radius - inner` thick.
func test_a_sheet_thinner_than_the_probe_spacing_reads_as_air(timeout := 300000) -> void:
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

	# ...and the occupancy grid cannot see it.
	var air := 0
	var solid := 0
	var cells := 0
	var probe_mins := []
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
					# ve::brick_probe, recomputed here: 3^3 samples at 0.4 m spacing. How far
					# does the closest of them clear zero? That is the pad the air verdict
					# would need in order to catch this cell.
					var mn := 1e30
					for sx in range(3):
						for sy in range(3):
							for sz in range(3):
								mn = minf(mn, w.debug_field_sdf(Vector3(
									(cx * 0.8) + sx * 0.4, (cy * 0.8) + sy * 0.4,
									(cz * 0.8) + sz * 0.4)))
					probe_mins.append(mn)
				else:
					solid += 1
	prints("cells holding matter:", cells, " reported AIR by the 3^3 probe:", air,
		" reported solid/full:", solid)
	probe_mins.sort()
	if probe_mins.size() > 0:
		prints("probe_mn over the missed cells: min %.4f  median %.4f  max %.4f" % [
			probe_mins[0], probe_mins[probe_mins.size() / 2],
			probe_mins[probe_mins.size() - 1]])
		var within := 0
		for m in probe_mins:
			if m <= 0.15:
				within += 1
		prints("missed cells that ACTIVATION_PAD (0.15) would catch:", within, "/",
			probe_mins.size())
	assert_int(air).override_failure_message(
		"the 3^3 probe saw every cell of a 10 cm skin; hypothesis is wrong").is_greater(0)
