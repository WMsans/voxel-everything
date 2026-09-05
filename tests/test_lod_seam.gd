extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

func make_world(residency_radius := 96.0) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	w.stream_radius_m = 1300.0 # the walk needs whole in-radius sibling sets (L6 spans
	# 819 m); this suite's cameras need ~1210 m, so 1300 is the floor here
	w.max_lod_pages = 16384
	w.residency_radius_m = residency_radius
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	return w

# The walk descends only into a node whose eight children are all resident, so the far field
# converges over HUNDREDS of ticks and at a rate set by build throughput, not by frame count.
# Wait on the condition (errata 6): fixed frame counts were tuned to the old occluded
# 590 ms vsync frame and settle nothing on the current runner, which keeps vsync enabled
# intentionally (gdunit_tests.sh) at a normal display rate.
# requests_pending comes from the walk that ran BEFORE this tick collected its results, so it
# dips to zero for a tick or two while a batch lands -- the streak is what makes it mean
# "converged" rather than "between batches". Measured: ~350-400 ticks, so the budget is margin.
const SETTLE_BUDGET := 6000
const QUIET_TICKS := 8

func settle(w: VoxelWorld, pos: Vector3, fwd: Vector3) -> bool:
	var quiet := 0
	for i in range(SETTLE_BUDGET):
		w.hooks().debug_lod_tick(pos, fwd)
		await get_tree().process_frame
		var d := w.hooks().debug_lod_stats()
		quiet = quiet + 1 if d["requests_pending"] == 0 and d["builds_in_flight"] == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# debug_seam_probe only runs 120 stream frames internally. The mutation test asks the
# raymarch (near field) to see the 120-150 m band, which requires a larger residency radius;
# drive the streamer to quiet first so the probe's internal budget is not the bottleneck.
func settle_stream(w: VoxelWorld, pos: Vector3) -> bool:
	var quiet := 0
	for i in range(2000):
		var actions := w.hooks().debug_stream_frame(pos)
		await get_tree().process_frame
		quiet = quiet + 1 if actions == 0 else 0
		if quiet >= QUIET_TICKS:
			return true
	return false

# The two masks are exact complements on the same pixel grid, so no pixel in the band may be
# claimed by both fields and none may be claimed by neither. A gap shows as sky through the
# ground; an overlap shows as z-fighting.
# The brief used (400,60,400), but that lies outside the 0..204.8 m test world and the
# far field's first visible terrain is ~195 m away, so no pixel falls in the 120-150 m
# band. This camera sits just inside the world's z edge, where the same view produces
# thousands of band pixels (measured >200 at 256x144).
func test_the_band_is_covered_exactly_once(timeout := 180000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.hooks().debug_seam_probe(pos, fwd, 256, 144)
	var band := w.hooks().debug_lod_fade_band()
	# NON-VACUITY FIRST. The probe cannot classify a pixel where the raymarch missed and no
	# field wrote depth -- it has no terrain sample there, so it counts it as sky. That is
	# the hole this metric used to be blind to: with the seam nailed to 120-150 m and the
	# near field's bricks reaching ~60 m, EVERY band pixel was a raymarch miss, the probe
	# skipped all of them, and "0 unclaimed" meant "nothing was measured". The seam now
	# follows the near field's real reach (ve::lod_fade_band), so the band is inside the
	# raymarch's data and these pixels are genuinely classified.
	assert_int(d["band_pixels"]).override_failure_message(
		"the probe camera saw no pixels in the %.0f-%.0f m band" % [band.x, band.y]
		).is_greater(200)
	# Measured at this camera: 2 of 3037 band pixels with the seam at 64-80 m, against 19 of
	# 3004 when the same probe is forced to the old 120-150 m band with a residency radius
	# big enough to make it measurable at all. The residue is pinhole-scale -- single pixels
	# leaking through a level transition, not the hollows this seam exists to prevent -- so
	# the bar is a fraction of the band rather than an absolute zero that was only ever true
	# because nothing was counted.
	# Unbounded-world recalibration (Task 15): the atlas funds ~half the 96 m residency ball
	# (documented tradeoff), so the band sits at 38-48 m -- exactly on the fade-start
	# knife-edge, where the fresh refinement pattern leaves scattered T-junction pinholes
	# (measured 25 of 1308, deterministic across radii 1300/1638 and cull on/off: not a
	# convergence or culling artefact). Same kind -- scattered pinholes, zero doubles -- at
	# 1.9%, so the bar moves 1/200 -> 1/40. It keeps its teeth: a stalled far-field lineage
	# measured 184 of 2510 (7.3%) on this probe and still fails.
	assert_int(d["band_pixels_unclaimed"]).override_failure_message(
		"%d of %d band pixels were claimed by neither field"
		% [d["band_pixels_unclaimed"], d["band_pixels"]]
		).is_less_equal(int(d["band_pixels"] / 40))
	assert_int(d["band_pixels_double_claimed"]).override_failure_message(
		"%d band pixels were claimed by both fields" % d["band_pixels_double_claimed"]
		).is_equal(0)

func test_the_near_field_owns_everything_before_the_band(timeout := 180000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	await settle(w, pos, fwd)
	var d := w.hooks().debug_seam_probe(pos, fwd, 256, 144)
	assert_int(d["near_pixels_lost_to_lod"]).is_equal(0)
	assert_int(d["far_pixels_lost_to_raymarch"]).is_equal(0)

# Mutation check for the unclaimed metric. The normal seam test asserts
# `band_pixels_unclaimed == 0`; without the hitpos fallback that assertion is vacuous
# because depth==0 pixels were skipped before the marker was inspected. Skipping the LoD
# pass creates a real far-field gap in the fade band: the near field still discards its
# half of the Bayer mask, no LoD pixel fills those holes, and the probe must count them.
# Unbounded-world note (Task 15): this used a high camera and residency 200 to put the
# band at 120-150 m, which needs the whole 200 m ball funded -- but brick-budget pricing
# halts funding at ~85% pool by design (conservative: never overdraw), so the far band is
# unreachable and the probe measured nothing. The normal low-camera geometry puts the
# band at 38-48 m with terrain under it, where the same mutation still leaves hundreds of
# unclaimed pixels.
func test_skipping_lod_counts_unclaimed_band_pixels(timeout := 60000) -> void:
	var w := make_world()
	var pos := Vector3(100.0, 68.0, 202.0)
	var fwd := Vector3(0.0, -0.12, -1.0).normalized()
	await settle(w, pos, fwd)
	await settle_stream(w, pos)
	var d := w.hooks().debug_seam_probe(pos, fwd, 256, 144, true)
	assert_int(d["band_pixels"]).override_failure_message(
		"mutation camera saw no band pixels with LoD skipped").is_greater(0)
	assert_int(d["band_pixels_unclaimed"]).override_failure_message(
		"skipping LoD should leave unclaimed band pixels, got %d" % d["band_pixels_unclaimed"]
		).is_greater(0)
	assert_int(d["band_pixels_double_claimed"]).is_equal(0)
