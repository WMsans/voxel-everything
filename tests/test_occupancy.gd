extends GdUnitTestSuite

# Spec section 5's occupancy grid, filled from the mark pass. The grid is what the flood fill
# reads, so if it disagrees with the field the wrong rocks fall — this test pins the two
# together on a streamed world and across an edit.
#
# CellState: 0 unknown, 1 air, 2 solid, 3 full (ve::CellState).
const UNKNOWN := 0
const AIR := 1
const SOLID := 2
const FULL := 3

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
	w.residency_radius_m = 30.0
	# Sizing rule of thumb (see test_streaming.gd): only regions CROSSING the surface hold
	# bricks, ~1500 each for the demo hills, and a 30 m ball holds a handful of them.
	w.atlas_bricks = Vector3i(32, 16, 32)
	w.max_region_slots = 64
	add_child(w)
	_worlds.append(w)
	assert_bool(w.debug_init_atlas()).is_true()
	return w

# The readback ring carries at most eight regions at a time and the request goes out one
# frame after the mark, so a freshly streamed ball takes a few dozen frames to be fully
# described. Settle until the block count stops growing.
func settle(w: VoxelWorld, center: Vector3, frames := 300) -> void:
	var last := -1
	var quiet := 0
	for i in range(frames):
		w.debug_stream_frame(center)
		var n: int = w.debug_occupancy_stats(center)["regions"]
		quiet = quiet + 1 if n == last else 0
		last = n
		if quiet >= 20 and n > 0:
			return

func test_the_grid_fills_in_around_the_camera(timeout := 60000) -> void:
	var w := make_world()
	assert_int(w.debug_occupancy_stats(CENTER)["regions"]).is_equal(0)
	settle(w, CENTER)
	assert_int(w.debug_occupancy_stats(CENTER)["regions"]).is_greater(2)

func test_every_described_cell_agrees_with_the_field(timeout := 60000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var rng := RandomNumberGenerator.new()
	rng.seed = 20260815
	var compared := 0
	var mismatched := 0
	var saw_air := 0
	var saw_full := 0
	var saw_solid := 0
	for i in range(600):
		var cell := Vector3i(rng.randi_range(10, 40), rng.randi_range(50, 80),
			rng.randi_range(10, 40))
		var gpu: int = w.debug_occupancy_state(cell)
		if gpu == UNKNOWN:
			continue # outside the streamed ball: nobody has looked yet, by design
		compared += 1
		if gpu != w.debug_cell_state(cell):
			mismatched += 1
		if gpu == AIR: saw_air += 1
		elif gpu == FULL: saw_full += 1
		else: saw_solid += 1
	assert_int(compared).is_greater(50)
	assert_int(mismatched).override_failure_message(
		"%d of %d described cells disagree with ve::cell_state_field" % [mismatched, compared]
		).is_equal(0)
	# All three states must actually occur, or the comparison proves nothing.
	assert_int(saw_air).is_greater(0)
	assert_int(saw_full).is_greater(0)
	assert_int(saw_solid).is_greater(0)

func test_an_edit_empties_the_cells_it_carves(timeout := 90000) -> void:
	var w := make_world()
	settle(w, CENTER)
	# A cell that is solidly underground before the edit.
	var cell := Vector3i(25, 62, 25)
	var before: int = w.debug_occupancy_state(cell)
	assert_int(before).is_not_equal(UNKNOWN)
	assert_int(before).is_not_equal(AIR)

	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# Centre the blast on the cell's own centre, radius 4 m: the cell and its 3^3 probe are
	# entirely inside it, so the only correct answer afterwards is AIR.
	tool.apply_sphere_subtract(Vector3(cell) * 0.8 + Vector3(0.4, 0.4, 0.4), 4.0)
	for i in range(120):
		w.debug_stream_frame(CENTER)
	assert_int(w.debug_occupancy_state(cell)).override_failure_message(
		"the carved cell is still reported as occupied").is_equal(AIR)
	assert_int(w.debug_occupancy_state(cell)).is_equal(w.debug_cell_state(cell))
	# ...and the block carries a sequence number at least as new as the edit.
	assert_int(w.debug_occupancy_stats(CENTER)["seq_at_center"]).is_greater_equal(1)

func test_occupancy_survives_a_region_being_evicted_and_reloaded(timeout := 90000) -> void:
	var w := make_world()
	settle(w, CENTER)
	var cell := Vector3i(25, 62, 25)
	var state: int = w.debug_occupancy_state(cell)
	assert_int(state).is_not_equal(UNKNOWN)
	# Walk far enough that the region is evicted, then come back.
	settle(w, CENTER + Vector3(200.0, 0.0, 0.0))
	# The grid is persistent (spec section 5): an evicted region keeps its block.
	assert_int(w.debug_occupancy_state(cell)).is_equal(state)
	settle(w, CENTER)
	assert_int(w.debug_occupancy_state(cell)).is_equal(state)

