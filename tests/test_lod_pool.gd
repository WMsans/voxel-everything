extends GdUnitTestSuite

var _worlds: Array = []

# The walk only descends into a node whose eight children are all resident, so the far field
# converges over hundreds of ticks. Every wait here is on a CONDITION with a wide budget,
# never on a frame count that happens to be long enough on one machine: these numbers are
# ceilings, and the cameras below reach their condition in ~350-400 ticks.
const SETTLE_BUDGET := 2500
# requests_pending is read from the walk that ran BEFORE this tick's results were collected,
# so it dips to zero for a tick or two while a batch is landing. Convergence has to hold for
# a streak or a settle stops before anything is built.
const QUIET_TICKS := 8

const NEAR_POS := Vector3(400.0, 70.0, 400.0)
const NEAR_FWD := Vector3(0, -0.2, -1)

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(pages: int = 256) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1000.0 # small but viable: the walk descends only into a node
	# whose eight children are all in-radius (L6 spans 819 m); below ~940 m it stalls
	# at 2 roots, so 1000 is the floor for these cameras
	w.max_lod_pages = pages
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# Ticks until the walk has nothing left to ask for and nothing is in flight. Returns the
# number of ticks it took, or -1 if the budget ran out (a pool too small to hold the view
# never converges -- it retries forever -- so callers that expect that pass their own loop).
func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> int:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return i + 1
	return -1

func test_the_pool_starts_empty_and_sized() -> void:
	var w := make_world(256)
	var d := w.hooks().debug_lod_stats()
	assert_int(d["pages_total"]).is_equal(256)
	assert_int(d["pages_free"]).is_equal(256)
	assert_int(d["chunks_resident"]).is_equal(0)

func test_ticking_streams_chunks_in(timeout := 120000) -> void:
	var w := make_world(16384)
	var ticks: int = await settle(w, NEAR_POS, NEAR_FWD)
	var d := w.hooks().debug_lod_stats()
	assert_int(ticks).override_failure_message(
		"the far field never converged: %s" % d).is_greater(0)
	assert_int(d["chunks_resident"]).override_failure_message(
		"settling produced no resident chunks: %s" % d).is_greater(0)
	assert_int(d["pages_free"]).is_less(16384)
	assert_int(d["draw_pages"]).override_failure_message(
		"chunks are resident but nothing is in the draw list").is_greater(0)

# M3 errata 5's lesson, restated for pages: a build that cannot get all its pages must be
# refused, never half-allocated. A tiny pool must degrade to a coarse world, not a broken one.
#
# This one cannot use settle(): a pool too small for the view refuses builds forever, so the
# walk never runs out of requests. It gets a fixed budget instead, and asserts the invariants
# that must hold WHILE the pool is starving.
func test_a_tiny_pool_degrades_to_coarse_instead_of_breaking(timeout := 60000) -> void:
	var w := make_world(24)
	for i in range(400):
		w.hooks().debug_lod_tick(NEAR_POS, NEAR_FWD)
		await get_tree().process_frame
	var d := w.hooks().debug_lod_stats()
	assert_int(d["pages_free"]).is_greater_equal(0)
	assert_int(d["pages_used"] as int + d["pages_free"] as int).is_equal(24)
	# debug_lod_stats measures this: a chunk holding a page with no quad count, or arena
	# pages no resident chunk owns. Both are what a half-funded build would leave behind.
	assert_int(d["partial_allocations"]).override_failure_message(
		"a build was partially funded: %s" % d).is_equal(0)
	# Something is still drawn: the coarse levels are exempt from eviction.
	assert_int(d["draw_pages"]).override_failure_message(
		"a starving pool drew nothing at all: %s" % d).is_greater(0)

func test_pages_come_back_when_chunks_are_evicted(timeout := 180000) -> void:
	var w := make_world(16384)
	assert_int(await settle(w, NEAR_POS, NEAR_FWD)).override_failure_message(
		"the near view never converged: %s" % w.hooks().debug_lod_stats()).is_greater(0)
	var used_near: int = w.hooks().debug_lod_stats()["pages_used"]
	assert_int(used_near).is_greater(0)
	# Jump far away and let the eviction age expire. Eviction is driven by the TICK counter
	# (kLodEvictFrames = 300 walks since a node was last touched), not by wall clock, so the
	# budget is in ticks and only needs to clear that age with room to spare.
	var used_far := used_near
	for i in range(900):
		w.hooks().debug_lod_tick(Vector3(1500.0, 400.0, 1500.0), Vector3(0, -1, 0))
		await get_tree().process_frame
		used_far = w.hooks().debug_lod_stats()["pages_used"]
		if used_far < used_near:
			break
	assert_int(used_far).override_failure_message(
		"nothing was ever evicted: pages_used %d -> %d" % [used_near, used_far]
		).is_less(used_near)
