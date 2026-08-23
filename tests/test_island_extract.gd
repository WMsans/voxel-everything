extends GdUnitTestSuite

# GPU/CPU differential test for island extraction (spec section 8): the world field
# intersected with a component's 0.8 m cell boxes, at 5 or 10 cm, on the mesher's worker
# device against ve::extract_island_volume.
#
# Tolerances follow tests/test_brick_diff.gd, and for the same reason: sin() is not
# bit-identical between glibc and a Vulkan driver, and a uint8 with ~5 mm steps cannot show a
# disagreement smaller than half a step. The MASK contributes no transcendentals at all, so
# a disagreement bigger than that is a real bug in the box arithmetic, not in libm.
const MAX_STEPS := 2

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
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_physics()).is_true() # starts the mesher's worker + its device
	return w

func check_extract(w: VoxelWorld, lo: Vector3i, hi: Vector3i, label: String) -> Dictionary:
	var d: Dictionary = w.hooks().debug_island_extract_diff(lo, hi)
	assert_bool(d.get("ok", false)).override_failure_message(
		"%s: extraction failed: %s" % [label, d]).is_true()
	assert_int(d["worst_steps"]).override_failure_message(
		"%s: worst sdf disagreement %d encoded steps" % [label, d["worst_steps"]]
		).is_less(MAX_STEPS)
	assert_int(d["mat_mismatch"]).override_failure_message(
		"%s: %d material mismatches" % [label, d["mat_mismatch"]]).is_equal(0)
	var dim: int = int(d.get("dim", 64))
	var expected := dim * dim * dim
	assert_int(int(d.get("normal_count", 0))).override_failure_message(
		"%s: expected %d normals got %s" % [label, expected, d.get("normal_count", 0)]).is_equal(expected)
	assert_float(float(d.get("normal_min_length", 0.0))).override_failure_message(
		"%s: normal length too small %s" % [label, d.get("normal_min_length", 0.0)]).is_greater(0.99)
	assert_float(float(d.get("normal_min_alignment", 0.0))).override_failure_message(
		"%s: normal alignment too low %s" % [label, d.get("normal_min_alignment", 0.0)]).is_greater(0.98)
	return d

func test_a_single_cell_extracts_to_the_same_volume_on_both_sides(timeout := 60000) -> void:
	# Deep underground: the box is entirely full, so the island is the box.
	var d := check_extract(make_world(), Vector3i(10, 20, 20), Vector3i(10, 20, 20), "cell")
	assert_int(d["gpu_solid"]).is_greater(0)
	assert_float(float(d["gpu_solid"]) / float(d["cpu_solid"])).is_between(0.99, 1.01)
	assert_float(d["voxel"]).is_equal_approx(0.05, 0.001)

func test_a_multi_cell_component_across_the_surface_matches(timeout := 60000) -> void:
	# Cells straddling the terrain surface near (20, 20): the interesting case, because the
	# mask and the field both have something to say about the same voxels.
	var d := check_extract(make_world(), Vector3i(24, 62, 24), Vector3i(26, 65, 26), "slab")
	assert_int(d["gpu_solid"]).is_greater(0)
	assert_float(float(d["gpu_solid"]) / float(d["cpu_solid"])).is_between(0.99, 1.01)
	# Four cells across is 3.2 m, past the fine pitch's reach: the planner drops to 10 cm.
	assert_float(d["voxel"]).is_equal_approx(0.10, 0.001)

func test_the_mask_cuts_the_terrain_at_the_box_faces(timeout := 60000) -> void:
	var w := make_world()
	var d := check_extract(w, Vector3i(10, 20, 20), Vector3i(11, 20, 20), "mask")
	# The extraction is an intersection, so the solid count can never exceed the boxes'
	# volume: two 0.8 m cells at 5 cm is 2 * 16^3 = 8192 voxels.
	assert_int(d["gpu_solid"]).is_less_equal(8192)
	# ...and here, where the terrain is solid throughout, it should very nearly reach it.
	assert_int(d["gpu_solid"]).is_greater(7000)

func test_an_edit_inside_the_component_reaches_the_extraction(timeout := 60000) -> void:
	var w := make_world()
	var before := check_extract(w, Vector3i(10, 20, 20), Vector3i(11, 20, 20), "before")
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# Carve half the box away. The extraction reads the region's op list, so both sides must
	# see it -- this is what catches an op pool that was uploaded to only one of them.
	tool.apply_sphere_subtract(Vector3(8.4, 16.4, 16.4), 0.6)
	var after := check_extract(w, Vector3i(10, 20, 20), Vector3i(11, 20, 20), "after")
	assert_int(after["gpu_solid"]).is_less(before["gpu_solid"])
	assert_int(after["gpu_solid"]).is_greater(0)

func test_a_component_crossing_a_region_boundary_sees_ops_from_both_regions(timeout := 60000) -> void:
	var w := make_world()
	# Bricks 31 and 32 straddle the region boundary at brick 32 (25.6 m). The component is
	# only two cells, smaller than a region, but its extraction must collect op lists from
	# both sides of that boundary.
	var before := check_extract(w, Vector3i(31, 20, 20), Vector3i(32, 20, 20), "cross-before")
	var tool: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(tool)
	# A small subtract entirely inside region 1 (x > 25.6 m) and inside the component's
	# second cell. With the old one-region capture the extraction looked only at region 0 and
	# would not see this op at all.
	tool.apply_sphere_subtract(Vector3(26.2, 16.8, 16.8), 0.2)
	var after := check_extract(w, Vector3i(31, 20, 20), Vector3i(32, 20, 20), "cross-after")
	assert_int(after["gpu_solid"]).override_failure_message(
		"a region-1 subtract did not reach a component that straddles the boundary"
		).is_less(before["gpu_solid"])
	assert_int(after["gpu_solid"]).is_greater(0)
