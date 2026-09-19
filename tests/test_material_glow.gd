extends GdUnitTestSuite

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
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	settle(w)
	return w

# Stream the world from the edit's own centre until it goes quiet. Used for the
# post-initialisation frame only now; see paint_and_settle for why a quiet window
# stopped being a sufficient sync for PAINTS.
func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break

# Paint, then pump until the SHIPPING marcher's material byte at the probe centre actually
# reads the painted id. The quiet-based settle above stopped syncing paints when the spec §4
# height band landed (ff8e743): pre-band, tree voxels kept the streamer busy around this
# centre long enough for the paint's atlas re-upload to finish inside the quiet window; the
# band removed 98.65 % of the trees (probe-item3 notes) and the window started exiting while
# the paint was still in flight. What actually lands the upload is a RENDERED frame -- the
# atlas upload advances with the rendered frame, not the stream tick -- so one pump round is
# 120 centre-stream frames plus one rendered headless probe, and the sync check reads the
# marched material back from the renderer (debug_raymarch_gbuffer), never from the CPU store
# the paint hook writes. Measured on this worktree: a paint queued while the previous paint's
# upload is still in flight marches the new material by round ~4 (0.35 -> 0.41 -> 1.29 luma).
# This is a sync condition, not an assertion: on a propagation failure the loop exits on
# budget and the brightness asserts below still fail loudly.
func paint_and_settle(w: VoxelWorld, radius: float, material: int) -> void:
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), radius, material)
	for i in range(8):
		for k in range(120):
			w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0))
		w.hooks().debug_deferred_probe(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
		var g: Dictionary = w.hooks().debug_raymarch_gbuffer(Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0))
		if bool(g.get("hit", false)) and int(g.get("material", -1)) == material:
			return

func magma_id() -> int:
	for m in _worlds[0].material_table():
		if float(m["glow"]) > 0.0:
			return int(m["id"])
	return -1

# Paint a patch of terrain with the emissive material and look at it. The lit buffer is
# rgba16f, so the emissive term is visible as HDR above what the same geometry reads
# with a non-emissive material -- which is exactly what Godot's bloom keys off.
#
# NOTE: the plan draft used radius 6.0 at this centre, but the terrain surface here
# sits at y ~= 49.6, so that sphere floated above the ground and painted nothing; it
# also covered too little of the probe frame to move mean_luma. Radius 16 both reaches
# the surface and dominates the view, which is what these assertions actually need.
# Trees (Task 7): trunks crossing the top of the probe frame stood above the r=16
# sphere, so their unpainted bark caps diluted the emissive mean_luma (ratio fell to
# 1.33). Radius 28 covers the trunk caps inside the frame as well -- measured lit/dull
# 0.748/0.349 = 2.14 with this suite's exact settle (r=22 reads 1.42, still short) --
# the same paint-geometry tuning this NOTE already establishes as the suite's policy.
#
# Two worlds (final review wave): the dull and emissive frames are measured on separate,
# identically seeded worlds that each receive ONE first paint, not as a repaint sequence
# in one world. A debug sphere paint queued after a previous paint's atlas upload has
# already committed never propagates to the GPU at all -- the marched material and the lit
# buffer stay on the old paint through 20 full pump rounds (this wave's variant matrix:
# the same [stream+render+march] pump that converges a paint queued mid-upload is dead
# flat on mat=1 for a committed-then-repainted patch, and a second paint sits stale under
# plain settle on this build too, long settle included). First paints do land, and the two
# worlds' terrain is deterministic, so the frames share their geometry exactly and only the
# painted material differs -- which is what the ratio below asserts. This routes around a
# latent streamer/upload propagation defect (filed in the fix report), not around the
# property under test; the assertion itself is unchanged.
func test_an_emissive_material_is_brighter_than_a_dull_one() -> void:
	var dw := make_world()
	var id := magma_id()
	assert_int(id).override_failure_message(
		"no material in the table has a non-zero glow").is_greater(0)
	var pos := Vector3(20.0, 75.0, 20.0)
	var down := Vector3(0, -1, 0)

	paint_and_settle(dw, 28.0, 1) # dull
	var dull: Dictionary = dw.hooks().debug_deferred_probe(pos, down, 64, 64, 0)
	var lw := make_world()
	paint_and_settle(lw, 28.0, id) # emissive
	var lit: Dictionary = lw.hooks().debug_deferred_probe(pos, down, 64, 64, 0)

	assert_float(lit["mean_luma"]).override_failure_message(
		"the emissive material shaded no brighter than the dull one: glow is not applied"
		).is_greater(float(dull["mean_luma"]) * 1.5)

# Emission must survive as HDR. If it were clamped to 1.0 the term would still "work" in
# the lit buffer and then contribute nothing at all to Godot's glow, which thresholds above 1.
func test_emission_pushes_the_lit_buffer_above_one() -> void:
	var w := make_world()
	var id := magma_id()
	paint_and_settle(w, 16.0, id)
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	var c: Color = d["center"]
	assert_float(maxf(c.r, maxf(c.g, c.b))).override_failure_message(
		"emissive pixel peaked at %.3f: nothing will bloom" % maxf(c.r, maxf(c.g, c.b))
		).is_greater(1.0)

# A non-emissive material must pay nothing and change nothing.
func test_a_non_emissive_material_is_unchanged() -> void:
	var w := make_world()
	paint_and_settle(w, 16.0, 1)
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	var c: Color = d["center"]
	assert_float(maxf(c.r, maxf(c.g, c.b))).is_less(1.0)
