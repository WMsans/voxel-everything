extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction every other GPU suite in this repo uses (see tests/test_ssao.gd):
# a local rendering device, physics off, streamed until the chunk queue goes quiet.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_grass_pass_runs_and_reports_its_capacity() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["ran"]).is_true()
	assert_int(d["capacity"]).is_greater(0)
	# Nothing is placed yet.
	assert_int(d["blades"]).is_greater_equal(0)

func test_grass_settings_round_trip_through_the_store() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 25.0)
	assert_float(w.get_grass_value("reach_m")).is_equal_approx(25.0, 0.001)
	# Out-of-range values are clamped by the store, not rejected.
	w.set_grass_value("reach_m", 1.0e9)
	assert_float(w.get_grass_value("reach_m")).is_less_equal(256.0)

func test_disabling_grass_zeroes_the_pass() -> void:
	var w := make_world()
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["bricks"]).is_equal(0)
	assert_int(d["blades"]).is_equal(0)

func test_stage_one_finds_surface_bricks_under_the_camera() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	# Standing on terrain at (30, 56.2, 30) with a 40 m reach, the brick box must contain
	# resident surface bricks. Zero here means the cull rejected everything.
	assert_int(d["bricks"]).is_greater(0)

func test_stage_one_finds_nothing_far_above_the_world() -> void:
	var w := make_world()
	# Far rings off: this is the NEAR box's contract. They are scattered against the field, so
	# they do find ground however high the camera is (test_ring_one_finds_ground_far_below_a_high_camera).
	w.set_grass_value("far_lod_rings", 0.0)
	# Stream around a point 2 km up: nothing is resident within the vertical reach.
	for i in range(30):
		w.hooks().debug_stream_frame(Vector3(30.0, 2000.0, 30.0))
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["bricks"]).is_equal(0)

func test_a_shorter_reach_culls_more_bricks() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 40.0)
	var wide: int = w.hooks().debug_grass_stats()["bricks"]
	w.set_grass_value("reach_m", 5.0)
	var narrow: int = w.hooks().debug_grass_stats()["bricks"]
	assert_int(narrow).is_less(wide)

func test_blades_appear_on_grass_terrain() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_greater(0)

# Ring boundaries are absolute 10 m steps now (ve::kGrassRingStepM), so a shorter reach no
# longer thins the grass that stays -- it culls ground. The hook camera looks straight down
# from about a metre up, so the reach has to fall INSIDE that view cone to remove anything:
# 12 m and 40 m both cover every brick the 90-degree frustum can see, at the same density.
func test_blade_count_falls_as_the_reach_shrinks() -> void:
	var w := make_world()
	w.set_grass_value("reach_m", 40.0)
	var near_stats: Dictionary = w.hooks().debug_grass_stats()
	var near_count: int = near_stats["blades"]
	w.set_grass_value("reach_m", 2.0)
	var far_stats: Dictionary = w.hooks().debug_grass_stats()
	assert_int(far_stats["blades"]).is_less(near_count)

func test_density_follows_blades_per_brick() -> void:
	var w := make_world()
	w.set_grass_value("blades_per_brick", 16.0)
	var dense_stats: Dictionary = w.hooks().debug_grass_stats()
	var dense: int = dense_stats["blades"]
	w.set_grass_value("blades_per_brick", 4.0)
	var sparse_stats: Dictionary = w.hooks().debug_grass_stats()
	assert_int(sparse_stats["blades"]).is_less(dense)

func test_the_blade_count_clamps_at_capacity_instead_of_overflowing() -> void:
	var w := make_world()
	w.set_grass_value("max_blades", 64.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_less_equal(64)
	# The high-water mark still reports what the frame WANTED, so an overflow is visible
	# rather than silent.
	assert_int(d["high_water"]).is_greater_equal(d["blades"])

# Grass refuses steep surfaces. Raising the threshold past vertical must leave nothing.
func test_no_blades_survive_an_impossible_slope_threshold() -> void:
	var w := make_world()
	w.set_grass_value("slope_cos_min", 1.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_equal(0)

# The spec's placement contract: no blade may stand on a surface steeper than the slope
# threshold allows, and no blade may be taller than the settings permit.
func test_every_sampled_blade_stands_on_an_up_facing_surface() -> void:
	var w := make_world()
	w.set_grass_value("slope_cos_min", 0.55)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["sampled"]).is_greater(0)
	assert_float(d["min_normal_y"]).is_greater_equal(0.5)  # 0.55 less oct quantisation
	assert_float(d["max_height"]).is_less_equal(
		w.get_grass_value("blade_height_m") * (1.0 + w.get_grass_value("height_jitter")) * 1.15 + 0.001)

# Twenty-seven: four quads along the Bezier profile plus the tip triangle. Two segments
# cannot bend; they draw a stiff card with a kink, which is what the field used to be.
func test_the_raster_issues_twenty_seven_vertices_per_blade() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["drawn"]).is_true()
	assert_int(d["vertices"]).is_equal(d["blades"] * 27)

# The sun tests stream from above the SURFACE grass west of the usual spot. The hook camera
# looks straight down from the streamed centre, and at (30, 56.2, 30) its cone only reaches
# grass-floored cave pockets ~6 m under the hill -- which the sun march correctly reports as
# fully shadowed (measured: every blade 0.0), and which would make "sunlit" untestable.
const OPEN_GRASS := Vector3(20.0, 60.0, 30.0)

func stream_to(w: VoxelWorld, centre: Vector3) -> void:
	for i in range(60):
		w.hooks().debug_stream_frame(centre)

# Blades on open ground carry the terrain's own sun visibility, marched once per blade by the
# scatter. Before this every blade wrote 1.0 and nothing near the camera could shadow grass.
func test_blades_on_open_ground_are_sunlit() -> void:
	var w := make_world()
	stream_to(w, OPEN_GRASS)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["sampled"]).is_greater(0)
	assert_float(d["mean_sun"]).is_greater_equal(0.95)

# A rock floating up-sun of the grass must shadow the blades beneath it. Placed along
# ve::kSunDir (cel.h) from the grass surface, with its underside well clear of the ground so
# it cannot grow or bury blades itself -- the only thing it can change is the sun march.
func test_an_occluder_up_sun_shadows_the_blades_beneath_it() -> void:
	var w := make_world()
	stream_to(w, OPEN_GRASS)
	var before: Dictionary = w.hooks().debug_grass_stats()
	assert_int(before["sampled"]).is_greater(0)
	var sun_dir := Vector3(0.5746958, 0.7662610, 0.2873479)
	w.hooks().debug_apply_sphere_add(Vector3(20.0, 53.5, 30.0) + sun_dir * 12.0, 5.0, 2)
	stream_to(w, OPEN_GRASS)
	var after: Dictionary = w.hooks().debug_grass_stats()
	assert_int(after["sampled"]).is_greater(0)
	assert_float(after["min_sun"]).is_less(0.1)
	assert_float(after["mean_sun"]).is_less(before["mean_sun"] - 0.1)

func test_no_blades_means_no_draw_but_not_a_failure() -> void:
	var w := make_world()
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["vertices"]).is_equal(0)
	assert_bool(d["ran"]).is_true()

# The edit-awareness contract from the design doc: the NEAR scatter reads the LIVE atlas, so
# an edit that takes grass away must take its blades on the next frame, with no invalidation
# code anywhere. Tested rather than assumed. This paints the whole reach to rock instead
# of digging a crater: a crater floor is fresh grass-band surface and legitimately grows
# NEW blades (74 -> 409 measured), which confounds removal with exposure. Painting moves
# only the material layer, so zero blades afterwards can only mean the scatter read it.
# (Hook name/signature verbatim from extension/src/debug/hooks.cpp:
# debug_apply_sphere_paint(centre, radius, material); rock is material id 2.)
#
# Far LoD rings are turned OFF here on purpose. They are scattered against eval_field, which
# has no override bricks bound, so they cannot see this paint -- and they reach eight times
# further than the 45 m sphere anyway. Their behaviour has its own case below.
func test_painting_grass_to_rock_removes_its_blades() -> void:
	var w := make_world()
	w.set_grass_value("far_lod_rings", 0.0)
	var before: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(before).is_greater(0)
	w.hooks().debug_apply_sphere_paint(Vector3(30.0, 50.0, 30.0), 45.0, 2)
	for i in range(60):
		w.hooks().debug_stream_frame(Vector3(30.0, 56.2, 30.0))
	var after: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(after).is_equal(0)

# Grass past the near field is the whole point of the far LoD rings: the brick atlas only
# holds full-resolution data for the first ~60 m, so without them the field stops where the
# brick sphere does. The hook camera sits at the streamed centre looking straight down, so
# streaming 60 m ABOVE the grass puts the ground outside the near field's reach SPHERE
# entirely -- every blade counted here is one the near field cannot place. The rings must find
# it against the field, and every one of those blades must stand on accepted ground.
func test_far_lod_rings_place_blades_past_the_near_reach() -> void:
	var w := make_world()
	stream_to(w, Vector3(30.0, 116.2, 30.0))
	w.set_grass_value("reach_m", 8.0)
	w.set_grass_value("far_lod_rings", 0.0)
	var near_only: Dictionary = w.hooks().debug_grass_stats()
	assert_int(near_only["blades"]).is_equal(0)
	w.set_grass_value("far_lod_rings", 3.0)
	var with_far: Dictionary = w.hooks().debug_grass_stats()
	# Cells, then blades: the far rings appear in the compacted list first.
	assert_int(with_far["bricks"]).is_greater(near_only["bricks"])
	assert_int(with_far["blades"]).is_greater(0)
	# ...and every far blade still stands on ground the slope test accepted.
	assert_float(with_far["min_normal_y"]).is_greater_equal(0.5)

# The near field's reach is a SPHERE around the camera, not a horizontal disc, so from high
# up its footprint on the ground shrinks and the far rings have to pick up the rest. They used
# to start at `reach` measured in XZ instead, which left a band of ground owned by NEITHER
# field: raising reach_m from high actually REMOVED blades (measured from 60 m: 15792 at 8 m,
# 11632 at 32 m), because the near field still could not reach the ground while the far ring's
# inner edge moved out. Giving the near field more reach may never thin the grass.
func test_grass_does_not_thin_as_the_reach_grows_from_high() -> void:
	var w := make_world()
	# 60 m above the grass at the usual spot: the near box's sphere no longer reaches the
	# ground, so every blade counted below is a far-ring blade.
	stream_to(w, Vector3(30.0, 116.2, 30.0))
	w.set_grass_value("reach_m", 8.0)
	var narrow: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(narrow).is_greater(0)
	w.set_grass_value("reach_m", 32.0)
	var wide: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(wide).is_greater_equal(narrow)

# Bullet-proofing the ring-1 case: from 200 m up, ring 1's own search span used to be
# `cam.y ± outer` (80 m), which sits entirely ABOVE the ground -- so it placed nothing under
# the camera while the outer rings, whose larger radius reaches further down, kept drawing
# further out. With only ring 1 enabled and a reach the near field cannot possibly cross, any
# blade here has to have come from ring 1 finding ground 200 m below it.
func test_ring_one_finds_ground_far_below_a_high_camera() -> void:
	var w := make_world()
	stream_to(w, Vector3(30.0, 256.2, 30.0))
	w.set_grass_value("reach_m", 8.0)
	w.set_grass_value("far_lod_rings", 1.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_greater(0)

# A far ring is scattered against the procedural field with no override bricks bound, so it
# is blind to edits by construction (GrassSettings::far_lod_rings). Pinned, not assumed: the
# day someone binds the override pool into stage 1, this is the case that says so.
func test_far_lod_rings_do_not_see_edits() -> void:
	var w := make_world()
	# Stream high enough that the near field cannot reach the ground, so the blades this case
	# counts are far-ring ones by construction (same setup as the reach-growth case).
	stream_to(w, Vector3(30.0, 116.2, 30.0))
	w.set_grass_value("reach_m", 8.0)
	var before: int = w.hooks().debug_grass_stats()["blades"]
	assert_int(before).is_greater(0)
	w.hooks().debug_apply_sphere_paint(Vector3(30.0, 50.0, 30.0), 45.0, 2)
	stream_to(w, Vector3(30.0, 116.2, 30.0))
	assert_int(w.hooks().debug_grass_stats()["blades"]).is_greater(0)

# Disabling grass on a world that already drew must REALLY stop the draw: the disabled
# run() clears the GPU draw args (not just the CPU counters), so the raster issues an
# empty indirect draw instead of re-drawing the previous frame's frozen blades. Same
# world throughout -- a fresh world proves nothing, its buffers read back as zero anyway.
func test_disabling_grass_after_it_drew_issues_an_empty_draw() -> void:
	var w := make_world()
	assert_int(w.hooks().debug_grass_stats()["blades"]).is_greater(0)
	w.set_grass_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_int(d["blades"]).is_equal(0)
	assert_int(d["vertices"]).is_equal(0)
	assert_bool(d["drawn"]).is_true()

# S7 (docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md §3.5). Blades
# used to write material 1 -- grass_01, the ground they grow on -- so an emissive grass_01
# would light every blade. They write their own foliage id instead. 200 is ve::kFoliageBase
# (extension/src/world/material_table.h) and grass_blade is its first row.
const MAT_GRASS_BLADE := 200

func test_blades_write_the_grass_blade_material_not_the_terrain_they_grow_on() -> void:
	var w := make_world()
	stream_to(w, OPEN_GRASS)
	var d: Dictionary = w.hooks().debug_grass_stats()
	assert_bool(d["drawn"]).is_true()
	var ids: PackedInt32Array = d["blade_materials"]
	assert_int(ids.size()).override_failure_message(
		"the hooked raster covered no pixels").is_greater(0)
	assert_array(Array(ids)).override_failure_message(
		"blade material ids: %s" % [ids]).is_equal([MAT_GRASS_BLADE])
