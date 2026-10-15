extends GdUnitTestSuite

# Fjords conifers (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md). The stage half
# reads the CPU field through VoxelWorld.raycast; the pass half (Tasks 6-7) reads what the
# shipping passes wrote through debug_conifer_stats.
const FJORDS := "res://assets/pipelines/fjords.pipeline"
const WATER_Y := 51.2
# fjords.pipeline: shore 2 (+1), snow line 380 - jitter 25 - ridge drop 40 - margin 40.
const SHORE_E := 3.0
const TREELINE_E := 275.0
# A disc reaches 1.15 x 1.25 x 3 m from its foot; on a slope-1.7 wall that is 7.3 m of height.
const DISC_DROP := 8.0

var _world: VoxelWorld

func after_test() -> void:
	if is_instance_valid(_world):
		_world.free()

func _open(seed := 0) -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	_world.terrain_pipeline_path = FJORDS
	_world.world_seed = seed
	_world.use_local_device = true
	_world.physics_enabled = false
	_world.stream_radius_m = 2600.0
	add_child(_world)
	assert_bool(_world.hooks().debug_init_atlas()).is_true()
	assert_int(_world.hooks().debug_pump_sectors(Vector3.ZERO, 1600.0, 900)).is_greater(0)

func _material(name: String) -> int:
	var names: Array = _world.material_table().map(func(m): return m["name"])
	return names.find(name) + 1

# Forest hits on a 61 x 61 grid at 25 m over +-750 m.
func _forest_hits() -> Array:
	var forest := _material("forest")
	var out := []
	for i in range(-30, 31):
		for j in range(-30, 31):
			var hit: Dictionary = _world.raycast(Vector3(i * 25.0, 800.0, j * 25.0), Vector3.DOWN, 1600.0)
			if hit["hit"] and int(hit["material"]) == forest:
				out.append(hit["pos"])
	return out

func test_the_forest_floor_appears_on_the_walls() -> void:
	_open()
	var hits := _forest_hits()
	assert_int(hits.size()).override_failure_message("no forest material in a 1.5 km grid").is_greater(20)

# Spec §4.1: no tree stands in the shore band or above the tree line. A disc spreads from its
# foot by up to DISC_DROP metres of height on the steepest permitted wall.
func test_the_forest_floor_stays_between_shore_and_tree_line() -> void:
	_open()
	for p in _forest_hits():
		var e: float = p.y - WATER_Y
		assert_float(e).override_failure_message("forest at e=%.1f, %s" % [e, p]).is_greater(SHORE_E - DISC_DROP)
		assert_float(e).override_failure_message("forest at e=%.1f, %s" % [e, p]).is_less(TREELINE_E + DISC_DROP)

# Trunks are bark and stand up out of the forest floor: a horizontal ray at 6 m above a
# forest hit, cast across its cell, meets bark somewhere in the grid.
func test_trunks_are_bark() -> void:
	_open()
	var bark := _material("bark")
	var seen := 0
	for p in _forest_hits():
		for dir in [Vector3.RIGHT, Vector3.LEFT, Vector3.FORWARD, Vector3.BACK]:
			var hit: Dictionary = _world.raycast(p + Vector3(0.0, 6.0, 0.0) - dir * 6.0, dir, 12.0)
			if hit["hit"] and int(hit["material"]) == bark:
				seen += 1
	assert_int(seen).override_failure_message("no ray met a trunk near any forest floor").is_greater(0)

# The pass half. Stream around a forest floor hit, then ask the SHIPPING passes (driven
# through VoxelFrame::draw_conifers, the compositor's own block) what they placed.
func _forest_spot() -> Vector3:
	var hits := _forest_hits()
	assert_int(hits.size()).is_greater(0)
	return hits[hits.size() / 2]

func _stream_at(p: Vector3) -> void:
	_world.hooks().debug_pump_sectors(p, 900.0, 600)
	var quiet := 0
	for i in range(600):
		quiet = quiet + 1 if _world.hooks().debug_stream_frame(p + Vector3(0.0, 20.0, 0.0)) == 0 else 0
		if quiet >= 6:
			break

func test_the_conifer_pass_places_cards_near_a_forest() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	var d: Dictionary = _world.hooks().debug_conifer_stats(spot + Vector3(0.0, 60.0, 0.0), Vector3(0.0, -1.0, 0.001))
	assert_bool(d["ran"]).is_true()
	assert_int(d["card_trees"]).is_greater(0)
	assert_int(d["clumps"]).is_greater(0)
	assert_int(d["card_vertices"]).is_equal(int(d["clumps"]) * 6)

func test_conifer_settings_round_trip_and_disable() -> void:
	_open()
	_world.set_conifer_value("card_reach_m", 180.0)
	assert_float(_world.get_conifer_value("card_reach_m")).is_equal_approx(180.0, 0.001)
	var spot := _forest_spot()
	_stream_at(spot)
	_world.set_conifer_value("enabled", 0.0)
	var d: Dictionary = _world.hooks().debug_conifer_stats(spot + Vector3(0.0, 60.0, 0.0), Vector3(0.0, -1.0, 0.001))
	assert_int(d["card_trees"]).is_equal(0)
	assert_int(d["impostors"]).is_equal(0)
	assert_int(d["clumps"]).is_equal(0)

# Every listed card tree is within the card reach (plus a crown) of the camera.
func test_card_trees_lie_inside_the_card_reach() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	_world.set_conifer_value("card_reach_m", 120.0)
	var eye := spot + Vector3(0.0, 60.0, 0.0)
	var d: Dictionary = _world.hooks().debug_conifer_stats(eye, Vector3(0.0, -1.0, 0.001))
	var recs: PackedFloat32Array = d["card_records"]
	assert_int(recs.size()).is_greater(0)
	for i in range(recs.size() / 4):
		var foot := Vector3(recs[i * 4], recs[i * 4 + 1], recs[i * 4 + 2])
		var centre := foot + Vector3(0.0, recs[i * 4 + 3] * 0.5, 0.0)
		assert_float(centre.distance_to(eye)).is_less(120.0 + recs[i * 4 + 3] * 0.5 + 1.0)

# Spec §5.1 step 3: painting every listed trunk away drops it from the list. Paint, not dig,
# for the reason test_leaves.gd gives; the reach is pulled in so every listed tree is resident.
func test_painting_trunks_away_empties_the_card_list() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	_world.set_conifer_value("card_reach_m", 40.0)
	var eye := spot + Vector3(0.0, 30.0, 0.0)
	var d: Dictionary = _world.hooks().debug_conifer_stats(eye, Vector3(0.0, -1.0, 0.001))
	var recs: PackedFloat32Array = d["card_records"]
	assert_int(recs.size()).is_greater(0)
	for i in range(recs.size() / 4):
		var anchor := Vector3(recs[i * 4], recs[i * 4 + 1] + recs[i * 4 + 3] * 0.33, recs[i * 4 + 2])
		_world.hooks().debug_apply_sphere_paint(anchor, 1.5, 2) # material 2 is rock
	for i in range(40):
		_world.hooks().debug_stream_frame(spot + Vector3(0.0, 20.0, 0.0))
	assert_int(_world.hooks().debug_conifer_stats(eye, Vector3(0.0, -1.0, 0.001))["card_trees"]).is_equal(0)

# Spec §6: from high above a forest, past the card reach, imposters draw.
func test_an_aerial_view_draws_impostors() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	var d: Dictionary = _world.hooks().debug_conifer_stats(spot + Vector3(-400.0, 450.0, 0.0), Vector3(1.0, -1.0, 0.0))
	assert_int(d["impostors"]).is_greater(0)
	assert_int(d["impostor_vertices"]).is_equal(int(d["impostors"]) * 6)

func test_impostors_stop_at_their_reach() -> void:
	_open()
	var spot := _forest_spot()
	_stream_at(spot)
	var eye := spot + Vector3(-400.0, 450.0, 0.0)
	var many: int = _world.hooks().debug_conifer_stats(eye, Vector3(1.0, -1.0, 0.0))["impostors"]
	_world.set_conifer_value("impostor_reach_m", 400.0)
	var few: int = _world.hooks().debug_conifer_stats(eye, Vector3(1.0, -1.0, 0.0))["impostors"]
	assert_int(few).is_less(many)

# The brick generator walks the conifer cells from a per-brick cache (conifer_walk_cached in
# stages/conifers.field.glslh), which must answer exactly as conifer_walk does. The bricks
# around a trunk, generated on the GPU, against ve::eval_brick running the CPU mirror -- the
# test_brick_diff.gd comparison, on the one stage that has a cache.
func test_bricks_around_a_trunk_match_the_cpu_reference() -> void:
	_open()
	var bark := _material("bark")
	var trunk := Vector3.INF
	for p in _forest_hits():
		for dir in [Vector3.RIGHT, Vector3.LEFT, Vector3.FORWARD, Vector3.BACK]:
			var hit: Dictionary = _world.raycast(p + Vector3(0.0, 6.0, 0.0) - dir * 6.0, dir, 12.0)
			if hit["hit"] and int(hit["material"]) == bark:
				trunk = hit["pos"]
				break
		if trunk != Vector3.INF:
			break
	assert_bool(trunk != Vector3.INF).override_failure_message("no trunk found").is_true()
	# A box of bricks around the trunk hit, inside the one region a mark may cover.
	var brick := Vector3i((trunk / 0.8).floor())
	var region := Vector3i((Vector3(brick) / 32.0).floor())
	var r0 := region * 32
	var r1 := r0 + Vector3i(31, 31, 31)
	var lo := (brick - Vector3i(6, 8, 6)).clamp(r0, r1)
	var hi := (brick + Vector3i(6, 12, 6)).clamp(r0, r1)
	_world.hooks().debug_mark_region(region, 0, lo, hi, 0, false)
	_world.hooks().debug_generate_pending()
	var jobs: PackedInt32Array = _world.hooks().debug_jobs()
	var bricks := []
	for j in range(jobs.size() / 8):
		bricks.append(Vector3i(jobs[j * 8 + 0], jobs[j * 8 + 1], jobs[j * 8 + 2]))
	# Nearest the trunk first: those hold the trunk itself and the forest floor at its foot.
	bricks.sort_custom(func(a, b): return Vector3(a - brick).length() < Vector3(b - brick).length())
	assert_int(bricks.size()).override_failure_message("no active bricks around the trunk").is_greater(8)
	for b in bricks.slice(0, 24):
		var d: Dictionary = _world.hooks().debug_brick_diff(b, 0, PackedByteArray(), 0)
		assert_int(d["slot"]).override_failure_message("brick %s is not resident" % b).is_greater_equal(0)
		assert_int(d["sdf_max_diff"]).override_failure_message(
			"brick %s sdf differs by %d encoded steps" % [b, d["sdf_max_diff"]]).is_less_equal(1)
		assert_bool(d["palette_match"]).override_failure_message("brick %s palette differs" % b).is_true()
		assert_int(d["mat_near_mismatch"]).override_failure_message(
			"brick %s has %d/%d near-surface material mismatches"
			% [b, d["mat_near_mismatch"], d["mat_near_compared"]]).is_equal(0)
