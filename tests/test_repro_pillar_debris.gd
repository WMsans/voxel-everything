extends GdUnitTestSuite

# DIAGNOSTIC ONLY (not a regression test): hunt the reported "cutting a pillar leaves tiny
# static slivers the falling half wedges against", by probing the FIELD directly and
# reporting any solid matter that is not connected to the ground.

const CENTER := Vector3(20.0, 56.0, 20.0)
const PILLAR_X := 20.4
const PILLAR_Z := 20.4
const PILLAR_BASE := 52.0

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
	w.residency_radius_m = 40.0
	w.atlas_bricks = Vector3i(48, 24, 48)
	w.max_region_slots = 64
	w.physics_radius_m = 30.0
	w.max_collider_chunks = 128
	w.shape_builds_per_frame = 4
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

func tool_of(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

func step(w: VoxelWorld, frames: int, center: Vector3 = CENTER) -> void:
	for i in range(frames):
		w.hooks().debug_stream_frame(center)
		w.hooks().debug_physics_frame(center)
		w.hooks().debug_island_frame(1.0 / 60.0, center)

func dump(tag: String, st: Dictionary) -> void:
	prints("  [%s]" % tag,
		"labelled", st["components_labelled"],
		"spawned", st["islands_spawned"], "debris", st["debris_spawned"],
		"crumbled", st["crumbled"], "refused", st["refused"],
		"(box_merge", st["refused_box_merge"], "lattice", st["refused_lattice"],
		"op_cap", st["refused_op_cap"], "body_cap", st["refused_body_cap"],
		"pool", st["refused_pool_full"], "unavail", st["refused_unavailable"],
		"empty", st["refused_empty"], "landing", st["refused_landing"], ")",
		"live", st["live_bodies"], "windows", st["pending_windows"],
		"runs", st["connectivity_runs"])
	if int(st["refused_landing"]) > 0:
		prints("    landing refusals:",
			"atlas_full", st["land_atlas_full"], "store", st["land_store_failed"],
			"no_log", st["land_no_edit_log"], "preflight", st["land_preflight"],
			"stale", st["land_stale"], "pin", st["land_pin_failed"],
			"spawn", st["land_spawn_failed"], "carve_nothing", st["land_carve_nothing"],
			"carve_restored", st["land_carve_restored"])

# Solid field samples on a `pitch` lattice, inset half a step so no sample lands on a 0.8 m
# cell face (where a carve box's own surface reads sdf == 0). Then 6-connectivity from the
# bottom layer: anything not reached is matter standing in mid-air.
#
# Returns [total_solid, floating_solid, floating_points].
func floating_matter(w: VoxelWorld, lo: Vector3, hi: Vector3, pitch := 0.1) -> Array:
	var nx := int((hi.x - lo.x) / pitch)
	var ny := int((hi.y - lo.y) / pitch)
	var nz := int((hi.z - lo.z) / pitch)
	var solid := PackedByteArray()
	solid.resize(nx * ny * nz)
	var total := 0
	for iy in range(ny):
		var y := lo.y + (iy + 0.5) * pitch
		for iz in range(nz):
			var z := lo.z + (iz + 0.5) * pitch
			var base := (iy * nz + iz) * nx
			for ix in range(nx):
				if w.hooks().debug_field_sdf(Vector3(lo.x + (ix + 0.5) * pitch, y, z)) < 0.0:
					solid[base + ix] = 1
					total += 1
	# Flood from the bottom layer (the terrain the box is anchored to).
	var seen := PackedByteArray()
	seen.resize(solid.size())
	var stack := PackedInt32Array()
	for iz in range(nz):
		for ix in range(nx):
			var i := (0 * nz + iz) * nx + ix
			if solid[i] == 1 and seen[i] == 0:
				seen[i] = 1
				stack.append(i)
	while stack.size() > 0:
		var i := stack[stack.size() - 1]
		stack.remove_at(stack.size() - 1)
		var ix := i % nx
		var iz := (i / nx) % nz
		var iy := i / (nx * nz)
		for d in range(6):
			var jx := ix + (1 if d == 0 else (-1 if d == 1 else 0))
			var jy := iy + (1 if d == 2 else (-1 if d == 3 else 0))
			var jz := iz + (1 if d == 4 else (-1 if d == 5 else 0))
			if jx < 0 or jx >= nx or jy < 0 or jy >= ny or jz < 0 or jz >= nz:
				continue
			var j := (jy * nz + jz) * nx + jx
			if solid[j] == 1 and seen[j] == 0:
				seen[j] = 1
				stack.append(j)
	var floaters := []
	for i in range(solid.size()):
		if solid[i] == 1 and seen[i] == 0:
			var ix2 := i % nx
			var iz2 := (i / nx) % nz
			var iy2 := i / (nx * nz)
			floaters.append(Vector3(lo.x + (ix2 + 0.5) * pitch, lo.y + (iy2 + 0.5) * pitch,
				lo.z + (iz2 + 0.5) * pitch))
	return [total, floaters.size(), floaters]

func report(w: VoxelWorld, tag: String) -> int:
	var r: Array = floating_matter(w, Vector3(PILLAR_X - 5.0, PILLAR_BASE - 1.0, PILLAR_Z - 5.0),
		Vector3(PILLAR_X + 5.0, PILLAR_BASE + 14.0, PILLAR_Z + 5.0))
	prints("  %s: solid samples %d, FLOATING %d" % [tag, r[0], r[1]])
	dump(tag, w.hooks().debug_island_stats())
	if r[1] > 0:
		# Bucket the floaters into 0.8 m cells so their size and shape are legible.
		var cells := {}
		for p in r[2]:
			var k := Vector3i(int(floor(p.x / 0.8)), int(floor(p.y / 0.8)), int(floor(p.z / 0.8)))
			cells[k] = int(cells.get(k, 0)) + 1
		prints("    floating occupancy cells:", cells.size())
		var shown := 0
		for k in cells:
			if shown >= 16:
				break
			prints("      cell", k, "grid", w.hooks().debug_occupancy_state(k),
				"field", w.hooks().debug_cell_state(k), "samples", cells[k])
			shown += 1
	return r[1]

# --- scenarios --------------------------------------------------------------------------

func stacked_pillar(w: VoxelWorld, t: VoxelEditTool, n: int, spacing: float,
		radius := 2.1) -> void:
	for i in range(n):
		t.apply_sphere_add(Vector3(PILLAR_X, PILLAR_BASE + spacing * i, PILLAR_Z), radius, 4)
	step(w, 120)

func test_diag_scenarios(timeout := 900000) -> void:
	# 1. The clean baseline: a densely stacked pillar, one big sphere cut.
	var w := make_world()
	w.hooks().debug_set_merge_sleep_seconds(999.0)
	var t := tool_of(w)
	stacked_pillar(w, t, 8, 1.4)
	t.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 4.9, PILLAR_Z), 3.0)
	step(w, 400)
	report(w, "1 dense stack + r3.0 cut")

	# 2. A scalloped pillar: adds spaced so the column is a string of lobes, which is what
	#    clicking RMB while walking actually builds.
	var w2 := make_world()
	w2.hooks().debug_set_merge_sleep_seconds(999.0)
	var t2 := tool_of(w2)
	stacked_pillar(w2, t2, 6, 3.2)
	t2.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 8.0, PILLAR_Z), 3.0)
	step(w2, 400)
	report(w2, "2 scalloped stack + r3.0 cut")

	# 3. The line drill through a dense pillar (demo R key: 10 x r0.6 along the aim ray).
	var w3 := make_world()
	w3.hooks().debug_set_merge_sleep_seconds(999.0)
	var t3 := tool_of(w3)
	stacked_pillar(w3, t3, 8, 1.4)
	var start := Vector3(PILLAR_X - 3.0, PILLAR_BASE + 4.9, PILLAR_Z)
	for i in range(10):
		t3.apply_sphere_subtract(start + Vector3(0.667 * i, 0, 0), 0.6)
	step(w3, 400)
	report(w3, "3 dense stack + line drill")

	# 4. A grazing cut: the sphere clips the side of the pillar instead of severing it.
	var w4 := make_world()
	w4.hooks().debug_set_merge_sleep_seconds(999.0)
	var t4 := tool_of(w4)
	stacked_pillar(w4, t4, 8, 1.4)
	t4.apply_sphere_subtract(Vector3(PILLAR_X + 2.6, PILLAR_BASE + 4.9, PILLAR_Z), 3.0)
	step(w4, 400)
	report(w4, "4 dense stack + grazing cut")

	# 5. Two cuts in quick succession, the second while the first is still resolving.
	var w5 := make_world()
	w5.hooks().debug_set_merge_sleep_seconds(999.0)
	var t5 := tool_of(w5)
	stacked_pillar(w5, t5, 8, 1.4)
	t5.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 4.9, PILLAR_Z), 3.0)
	step(w5, 6)
	t5.apply_sphere_subtract(Vector3(PILLAR_X, PILLAR_BASE + 8.0, PILLAR_Z), 2.0)
	step(w5, 400)
	report(w5, "5 dense stack + two cuts")
