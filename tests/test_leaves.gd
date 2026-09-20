extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# Same world construction every other GPU suite in this repo uses (see tests/test_grass.gd).
# Streams from (20, 60, 30) rather than the usual (30, 56.2, 30) hook view: that view sits
# inside the cave and sees only buried geometry, which is the wrong place to judge canopies.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 60.0, 30.0)) == 0 else 0
		if quiet >= 6:
			break
	return w

func test_the_leaf_pass_runs_and_reports_its_capacity() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_bool(d["ran"]).is_true()
	assert_int(d["capacity"]).is_greater(0)

func test_leaf_settings_round_trip_through_the_store() -> void:
	var w := make_world()
	w.set_leaf_value("reach_m", 180.0)
	assert_float(w.get_leaf_value("reach_m")).is_equal_approx(180.0, 0.001)
	w.set_leaf_value("reach_m", 1.0e9)
	assert_float(w.get_leaf_value("reach_m")).is_less_equal(400.0)

func test_disabling_leaves_zeroes_the_pass() -> void:
	var w := make_world()
	w.set_leaf_value("enabled", 0.0)
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["trees"]).is_equal(0)
	assert_int(d["clumps"]).is_equal(0)

func test_stage_one_finds_trees_near_the_camera() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["trees"]).is_greater(0)

func test_the_tree_list_shrinks_as_the_camera_retreats() -> void:
	var w := make_world()
	var near_count: int = w.hooks().debug_leaf_stats()["trees"]
	w.set_leaf_value("reach_m", 40.0)
	var far_count: int = w.hooks().debug_leaf_stats()["trees"]
	assert_int(far_count).is_less(near_count)

# THE headline contract. Painting the whole reach to rock removes every trunk's bark voxels
# without moving any geometry, so the stage-1 attachment check must drop every tree.
#
# Paint, NOT dig: digging a trunk exposes fresh ground around it and would confound removal
# with exposure. This is the same correction tests/test_grass.gd already had to make.
func test_painting_trunks_away_empties_the_tree_list() -> void:
	var w := make_world()
	var before: int = w.hooks().debug_leaf_stats()["trees"]
	assert_int(before).is_greater(0)
	# Material 2 is rock; see ve::kMaterials in extension/src/world/material_table.h.
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 60.0, 30.0), 60.0, 2)
	for i in range(40):
		w.hooks().debug_stream_frame(Vector3(20.0, 60.0, 30.0))
	assert_int(w.hooks().debug_leaf_stats()["trees"]).is_equal(0)

# THE PER-CLUMP HALF of the same contract. Stage 1 answers "does this tree still stand?"
# once, from one probe a third of the way up the trunk; a canopy used to inherit that one
# answer whole, so carving every branch out of a crown left all of its leaves hanging in the
# air. Stage 2 now re-asks at each clump's OWN wood -- the nearest point on the skeleton,
# read from the live atlas -- so this dig takes the leaves with it while the trunk below it,
# and therefore the tree's listing, survives. Digging, not painting: the point is that the
# tree is STILL LISTED and the clumps are gone anyway.
func test_carving_the_branches_out_of_a_crown_takes_its_leaves() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	var trees: int = int(d["trees"])
	assert_int(trees).is_greater(0)
	var recs: PackedFloat32Array = d["tree_records"]
	var before: int = int(d["clumps"])
	assert_int(before).is_greater(0)
	var best := 0
	for i in range(1, trees):
		if recs[i * 8 + 6] < recs[best * 8 + 6]:
			best = i
	var crown := Vector3(recs[best * 8], recs[best * 8 + 1], recs[best * 8 + 2])
	var crown_r: float = recs[best * 8 + 3]
	# Centred above the crown, so the sphere swallows every limb and the top of the trunk
	# but stops well short of the stage-1 anchor at base + 0.33 * height.
	w.hooks().debug_apply_sphere_subtract(crown + Vector3(0.0, crown_r * 0.6, 0.0), crown_r)
	for i in range(60):
		w.hooks().debug_stream_frame(Vector3(20.0, 60.0, 30.0))
	var after: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(int(after["trees"])).override_failure_message(
		"the dig reached the stage-1 trunk anchor; this case no longer tests stage 2"
		).is_equal(trees)
	assert_int(int(after["clumps"])).override_failure_message(
		"clumps survived a crown with no branches left in it").is_less(before)

func test_stage_two_places_clumps() -> void:
	var w := make_world()
	assert_int(w.hooks().debug_leaf_stats()["clumps"]).is_greater(0)

func test_the_clump_count_falls_as_the_reach_shrinks() -> void:
	var w := make_world()
	var far: int = w.hooks().debug_leaf_stats()["clumps"]
	w.set_leaf_value("reach_m", 60.0)
	var near: int = w.hooks().debug_leaf_stats()["clumps"]
	assert_int(near).is_less(far)

func test_every_clump_sits_inside_its_crown() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["sampled"]).is_greater(0)
	# THE ENVELOPE (R10, fix-round note): one metric, one story. counters.pad carries
	# (centre distance + card radius) / crown_r -- tree.glslh's containment invariant bounds
	# the CENTRE at 1.0 radii and the density LOD inflates the card (default-derived worst
	# whole-card reach 1.60), so 1.75 is the shipping envelope for the COMBINED word. This
	# is the single ≤1.75 guard the suite keeps: the centre-only ≤1.001 form cannot be
	# measured separately because the shader reports only the combined metric, so the two
	# halves of the story live in two cases -- here the card stays inside the envelope, in
	# test_the_crown_envelope_reports_the_card_radius the radius term is present at all.
	# Measured ~1.18.
	assert_float(d["max_crown_offset"]).is_less_equal(1.75)

# R10: counters.pad carries (centre distance + radius) / crown_r. A centre-only
# metric can never exceed 1.0 by Task 2's invariant, so this is the radius tooth made
# observable: drop the + radius term from the shader and this case fails, loudly, while
# the ≤1.75 envelope above stays green -- the two cases assert different things and neither
# duplicates the other (fix-round R11 note: the ≤1.75 half here was a copy of the envelope
# guard above and is removed; the shader reports one combined word, so it cannot have a
# separate centre-containment assertion).
func test_the_crown_envelope_reports_the_card_radius() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["sampled"]).is_greater(0)
	assert_float(d["max_crown_offset"]).is_greater(1.0)

func test_the_clump_count_clamps_at_capacity_instead_of_overflowing() -> void:
	var w := make_world()
	w.set_leaf_value("max_clumps", 64.0)
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["clumps"]).is_less_equal(64)
	assert_int(d["high_water"]).is_greater(0)

func test_zero_clumps_per_tree_places_nothing_but_still_finds_trees() -> void:
	var w := make_world()
	w.set_leaf_value("clumps_per_tree", 0.0)
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["clumps"]).is_equal(0)

func test_the_raster_draws_the_clumps_the_scatter_placed() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	assert_int(d["clumps"]).is_greater(0)
	# Six vertices per clump: two triangles per card.
	assert_int(d["vertices"]).is_equal(d["clumps"] * 6)

func test_disabling_leaves_draws_nothing() -> void:
	var w := make_world()
	w.set_leaf_value("enabled", 0.0)
	assert_int(w.hooks().debug_leaf_stats()["vertices"]).is_equal(0)

# THE SPEC §9 CONTRACT (arrived in the Task-15 fix wave, not as tests/test_trees.gd --
# see the deviation record): a known tree cell shows bark material in the G-buffer and a
# known clearing shows no tree. Everything is read from shipping output: the tree list
# and the dispatch grid debug_leaf_stats() reports are what the real scatter pass wrote,
# and the bark id comes from driving the real raymarch pass and reading its surface
# texture's id channel back (the same instrument test_raymarch_gbuffer.gd uses).
const MAT_BARK := 8 # shaders/material_table.glslh, ve::kMaterials

# Geometry of debug_leaf_stats()'s hook camera (see hooks_render.cpp): 40 m straight
# above the last streamed centre (20, 60, 30), looking straight down, 90-degree FOV.
# A candidate clearing cell within 21 m of that centre keeps its whole crown bounding
# sphere (crowns sit at most 51.2 + 4 + 11.25 m up under the spec §4 band gate, radius
# at most 4 m) inside the frustum footprint at crown depth (~33 m), inside the reach
# asserted below by a factor of more than three, and inside the funded frontier every
# Task 7 probe established around the stream centre. Distance, frustum and chop culls
# are therefore ruled out for it, so the only reason left for it to be absent from the
# tree list is the placement gate itself (hash/grove/slope/height band): a clearing.
const VIEW_CENTER := Vector2(20.0, 30.0)
const CLEARING_MAX_OFFSET := 21.0

func test_a_known_tree_shows_bark_in_the_gbuffer_and_a_known_clearing_shows_none() -> void:
	var w := make_world()
	var d: Dictionary = w.hooks().debug_leaf_stats()
	var trees: int = int(d["trees"])
	assert_int(trees).is_greater(0)
	var recs: PackedFloat32Array = d["tree_records"]
	assert_int(recs.size()).is_equal(trees * 8)
	var grid: PackedInt32Array = d["tree_grid"]
	assert_int(grid.size()).is_equal(5)
	assert_int(grid[4]).is_equal(grid[2] * grid[3]) # one thread per cell, as shipped
	var cell: float = float(d["tree_cell_m"])
	assert_float(cell).is_greater(0.0)
	assert_float(float(d["tree_reach_m"])).is_greater(100.0)

	# --- THE KNOWN TREE: the nearest listing. Position-distinct floats never tie
	# exactly, and compaction order is nondeterministic while the SET is not, so
	# "minimum distance" picks the same tree every run.
	var best := 0
	for i in range(1, trees):
		if recs[i * 8 + 6] < recs[best * 8 + 6]:
			best = i
	var crown := Vector3(recs[best * 8], recs[best * 8 + 1], recs[best * 8 + 2])
	var crown_r: float = recs[best * 8 + 3]
	var ground_y: float = recs[best * 8 + 4]
	assert_float(recs[best * 8 + 7]).is_greater_equal(1.0) # a listing keeps budget >= 1
	# t.crown = t.base + (0, height, 0) (shaders/tree.glslh), so the crown's XZ IS the
	# trunk axis, and bark is solid on that axis from the foot up to the crown centre.
	# Start above the bounding sphere -- every capsule sits within crown_r of the crown
	# centre -- so the origin is air, and the first thing this column owns is bark.
	var g: Dictionary = w.hooks().debug_raymarch_gbuffer(
		Vector3(crown.x, crown.y + crown_r + 1.0, crown.z), Vector3(0, -1, 0))
	assert_bool(g["hit"]).override_failure_message(
		"nothing hit down the trunk axis of the nearest listed tree at %s" % str(crown)
		).is_true()
	var hit: Vector3 = g["position"]
	assert_float(Vector2(hit.x - crown.x, hit.z - crown.z).length()).is_less(1.0)
	assert_float(hit.y).is_greater_equal(ground_y - 1.0)
	assert_float(hit.y).is_less(crown.y + crown_r)
	assert_int(g["material"]).override_failure_message(
		"the trunk column of a known tree cell did not report MAT_BARK in the G-buffer: %s" \
		% str(g)).is_equal(MAT_BARK)

	# --- THE KNOWN CLEARING: the first lattice cell of the shipped dispatch box, within
	# CLEARING_MAX_OFFSET of the view centre, that carries no listing (records sit within
	# +-0.35 cells of their centre, never near a +-0.5 boundary) AND no listing's crown
	# sphere within crown_r + 1 m of its centre, so no branch can overhang the column
	# either. Scan order is fixed, so the pick is deterministic.
	var found := false
	var c := Vector2.ZERO
	for k in range(grid[1], grid[1] + grid[3]):
		for i in range(grid[0], grid[0] + grid[2]):
			c = Vector2((float(i) + 0.5) * cell, (float(k) + 0.5) * cell)
			if c.distance_to(VIEW_CENTER) > CLEARING_MAX_OFFSET:
				continue
			var listed := false
			var clear_of_bark := true
			for j in range(trees):
				var axis := Vector2(recs[j * 8], recs[j * 8 + 2])
				if absf(axis.x - c.x) < cell * 0.5 and absf(axis.y - c.y) < cell * 0.5:
					listed = true
				if axis.distance_to(c) <= recs[j * 8 + 3] + 1.0:
					clear_of_bark = false
			if not listed and clear_of_bark:
				found = true
				break
		if found:
			break
	assert_bool(found).override_failure_message(
		"no unlisted dispatch cell near the view: the §9 clearing half needs one"
		).is_true()
	# Absence from the tree list -- both halves the brief allows, stated explicitly:
	# (1) this cell is not in the shipping tree list, and (2) its column carries no
	# bark id. The pick was chosen by these criteria; re-assert as the formal check.
	for j in range(trees):
		assert_bool(absf(recs[j * 8] - c.x) < cell * 0.5
				and absf(recs[j * 8 + 2] - c.y) < cell * 0.5) \
			.override_failure_message("the clearing cell is listed after all: %s" % str(c)) \
			.is_false()
		assert_float(Vector2(recs[j * 8], recs[j * 8 + 2]).distance_to(c)) \
			.is_greater(recs[j * 8 + 3] + 1.0)
	var air: Dictionary = w.hooks().debug_raymarch_gbuffer(Vector3(c.x, 78.0, c.y),
		Vector3(0, -1, 0))
	assert_bool(air["hit"]).override_failure_message(
		"no terrain under a known clearing cell at %s" % str(c)).is_true()
	assert_int(air["material"]).override_failure_message(
		"a known clearing cell reported MAT_BARK in the G-buffer: %s" % str(air) \
		).is_not_equal(MAT_BARK)
