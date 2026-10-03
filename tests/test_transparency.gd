extends GdUnitTestSuite

# Transparent voxels, end to end (docs/superpowers/specs/2026-10-01-transparent-voxels-
# design.md §9). Every frame is debug_render_frame -- the shipping VoxelFrame on a local
# device -- so what is asserted is what ships, never a hook's re-implementation of it.
#
# (20, 60, 30) is the open, sunlit meadow; the camera sits above it looking almost straight
# down, so the centre pixel is lit ground.

const CAM := Vector3(20.0, 64.0, 30.0)
const FWD := Vector3(0.15, -1.0, 0.1)
const W := 64
const H := 64

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# `enabled` is read where lattices are baked, so it is set BEFORE anything streams.
# `residency_m` shrinks the fade band with it (ve::lod_fade_band), which is how a test gets a
# deterministic seam instead of the measured streaming reach of whatever world it built.
func make_world(enabled := true, cam := CAM, residency_m := 0.0) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	if residency_m > 0.0:
		w.residency_radius_m = residency_m
	add_child(w)
	_worlds.append(w)
	w.set_transparency_value("enabled", 1.0 if enabled else 0.0)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	# Hold every temporal input still, so two frames of one view are one image. That is the
	# wind: ssgi is off and the grass and canopy wind speeds are zero (leaves STAY in the
	# scene -- only their motion is frozen). SSAO is off too, but NOT because it is stochastic:
	# shaders/ssao.comp.glsl is a pure function of the G-buffer -- a fixed 4x4 bayer4(px)
	# rotation, no frame counter, no history read -- and 8 back-to-back renders of one world
	# are bit-identical with it on. What varies is the WORLD: with ssao on, two identically
	# built and identically streamed worlds flip between exactly two lit_checksums (~50% of
	# pairs), and it does so with grass off AND with leaves removed entirely. That input is
	# still unidentified; see the Task 4 report. Grass is the other known one, because it
	# scatters blades with an atomicAdd, so blade ORDER -- not blade content -- depends on how
	# two dispatches happen to land, which changes the blend and so the checksum.
	w.set_effect_enabled("ssgi", false)
	w.set_effect_enabled("ssao", false)
	w.set_grass_value("enabled", 0.0)
	w.set_grass_value("wind_strength", 0.0)
	w.set_grass_value("wind_speed", 0.0)
	w.set_leaf_value("wind_strength", 0.0)
	w.set_leaf_value("wind_speed", 0.0)
	settle(w, cam)
	return w

func settle(w: VoxelWorld, cam := CAM) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(cam) == 0 else 0
		if quiet >= 6:
			break

func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

# Where the centre pixel's ray first meets the world (the CPU field: ice counts as solid).
func centre_hit(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var hit: Dictionary = w.raycast(cam, fwd.normalized(), 400.0)
	assert_bool(hit["hit"]).override_failure_message("the centre ray sees nothing").is_true()
	return hit

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), W, H)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

func finite(c: Color) -> bool:
	return is_finite(c.r) and is_finite(c.g) and is_finite(c.b)

# --- the opaque view in the marcher (spec §3) ---------------------------------------------

func test_the_marcher_sees_the_ground_through_an_added_ice_ball() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_add(ground["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).override_failure_message(
		"the G-buffer holds a transparent material: %s" % d).is_equal(int(before["center_material"]))
	# A transparent material is air, so the ball contributes NOTHING to the opaque bake and
	# the ground must come out where the bare frame found it: measured delta +0.000 m, the
	# same lattice either way. (Before the opaque accumulator this moved +0.43 m: the ball
	# won the min over the ground and the carve deleted the top half metre of it, leaving a
	# pit the marcher stopped in.) The ball's own surface is ~1 m NEARER than the ground --
	# the feature-off build measures 10.28 against 11.33 -- so a hit that moved at all would
	# be a real regression, not a nudge.
	var dist := float(d["center_distance"])
	assert_float(dist).override_failure_message(
		"the ground under the ice moved: %s" % d).is_equal_approx(
		float(before["center_distance"]), 0.05)

# The prior attempt went solid past 2.4 m of ice. Ten metres must change nothing here.
func test_ten_metres_of_ice_do_not_stop_the_marcher() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_add(ground["pos"] + Vector3(0, 4.0, 0), 5.0, material_id(w, "ice"))
	settle(w, CAM + Vector3(0, 12.0, 0))
	var cam := CAM + Vector3(0, 12.0, 0)
	var d := frame(w, cam)
	assert_int(int(d["center_material"])).is_equal(int(before["center_material"]))
	assert_float(float(d["center_distance"])).override_failure_message(
		"the hit is not ~12 m further than before: %s" % d).is_greater(
		float(before["center_distance"]) + 10.0)

# Painted ice relabels ground in place: the surface behind it is a LABEL boundary.
func test_painted_ice_does_not_move_the_ground_it_labels() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_paint(ground["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).is_not_equal(material_id(w, "ice"))
	assert_int(int(d["center_material"])).is_not_equal(0)
	# The label changed and the DISTANCE DID NOT. A painted lens is a relabel in place: the
	# surface behind it is a label boundary, and a transparent label is air, so the ground
	# stays exactly where it was -- measured delta +0.000 m (was +0.87 m, the same pit).
	assert_float(float(d["center_distance"])).override_failure_message(
		"painting the ground moved its surface: %s" % d).is_equal_approx(
		float(before["center_distance"]), 0.05)
	assert_bool(finite(d["center_lit"])).is_true()

func test_with_the_feature_off_ice_is_an_opaque_surface() -> void:
	var w := make_world(false)
	var ground := centre_hit(w)
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(ground["pos"], 1.0, ice)
	settle(w)
	assert_int(int(frame(w)["center_material"])).is_equal(ice)

func test_a_scene_without_transparency_is_bit_identical_with_the_feature_on_and_off() -> void:
	var on := frame(make_world(true))
	var off := frame(make_world(false))
	assert_int(int(on["lit_checksum"])).override_failure_message(
		"transparency changed a frame with no transparent material in it").is_equal(
		int(off["lit_checksum"]))

# --- the near shell (spec §5) -------------------------------------------------------------

func shell_chunk_of(p: Vector3) -> Vector3i:
	return Vector3i(floori(p.x / 3.2), floori(p.y / 3.2), floori(p.z / 3.2))

func test_a_shell_only_build_contours_just_the_ice() -> void:
	var w := make_world()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var ground := centre_hit(w)
	var c: Vector3 = ground["pos"] + Vector3(0, 0.5, 0)
	# No ice yet: the chunk is ground and air, and a shell-only build returns nothing.
	var none: Dictionary = w.hooks().debug_shell_build(shell_chunk_of(c))
	assert_bool(none["ok"]).is_true()
	assert_int(int(none["quads"])).is_equal(0)
	w.hooks().debug_apply_sphere_add(c, 1.0, material_id(w, "ice"))
	var d: Dictionary = w.hooks().debug_shell_build(shell_chunk_of(c))
	assert_bool(d["ok"]).is_true()
	# A 1 m ball at 0.1 m cells: 4*pi*r^2 / 0.01 ~ 1250 quads over the chunks it spans.
	assert_int(int(d["quads"])).is_greater(100)
	assert_bool(d["all_transparent"]).is_true()

# Frames until the near shell has pages, or gives up. Builds are async on the mesh worker.
func frame_until_shell(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var stats := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		frame(w, cam, fwd)
		stats = w.hooks().debug_lod_stats()
		if int(stats.get("shell_pages", 0)) > 0:
			break
	return stats

# The shell needs the MeshService (debug_init_physics) and the LoD pool (a LoD query), the
# same two preconditions every far-field suite sets up.
func shell_world(enabled := true, residency_m := 0.0) -> VoxelWorld:
	var w := make_world(enabled, CAM, residency_m)
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_stats()
	return w

func test_adding_ice_near_the_camera_publishes_shell_pages() -> void:
	var w := shell_world()
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).is_equal(0)
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var stats := frame_until_shell(w)
	assert_int(int(stats["shell_pages"])).override_failure_message(
		"no near-shell page was ever published: %s" % stats).is_greater(0)
	assert_int(int(stats["shell_chunks"])).is_greater(0)

func test_removing_the_ice_releases_its_shell_pages() -> void:
	var w := shell_world()
	var p: Vector3 = centre_hit(w)["pos"]
	w.hooks().debug_apply_sphere_add(p, 1.0, material_id(w, "ice"))
	settle(w)
	assert_int(int(frame_until_shell(w)["shell_pages"])).is_greater(0)
	w.hooks().debug_apply_sphere_subtract(p, 2.5)
	settle(w)
	var stats := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
		stats = w.hooks().debug_lod_stats()
		if int(stats["shell_pages"]) == 0:
			break
	assert_int(int(stats["shell_pages"])).override_failure_message(
		"the shell outlived the ice: %s" % stats).is_equal(0)
	# Nothing leaked: partial_allocations IS the leak detector here -- arena pages that no
	# resident chunk owns (a shell chunk owns its pages, and stats() counts them). A leak
	# makes it go UP; the old pages_free <= free_before form was one-sided and could not see
	# a leak at all, because a leak drives pages_free DOWN, which that form accepts.
	assert_int(int(stats["partial_allocations"])).override_failure_message(
		"the shell leaked pool pages: %s" % stats).is_equal(0)

# The other release path: the ice is still in the edit log, so its chunks are still CANDIDATES
# and their build comes back empty -- the pages come back only when the chunk leaves the
# candidate set and set_candidates hands it to `evicted`.
func test_moving_the_camera_out_of_the_radius_releases_the_shell_pages() -> void:
	var w := shell_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	assert_int(int(frame_until_shell(w)["shell_pages"])).is_greater(0)
	# Past kLodFadeEndM (150 m) plus one shell chunk, with the camera parked out there: no
	# edit, no toggle -- the candidate set is dropped by radius alone.
	var away := CAM + Vector3(220.0, 0.0, 0.0)
	var stats := {}
	for i in range(600):
		w.hooks().debug_stream_frame(away)
		frame(w, away)
		stats = w.hooks().debug_lod_stats()
		if int(stats["shell_pages"]) == 0:
			break
	assert_int(int(stats["shell_pages"])).override_failure_message(
		"the shell outlived the radius it was culled by: %s" % stats).is_equal(0)
	assert_int(int(stats["partial_allocations"])).override_failure_message(
		"the eviction released no pages: %s" % stats).is_equal(0)

# enabled is a bound property, so this toggle is live-reachable with the camera parked and no
# edit: neither direction is observable from the camera chunk or the dirty flag alone.
#
# WHAT THIS PINS: the shell's CANDIDATE SET and its pages are dropped and restored. That is
# what refresh_shell_candidates' `toggled` term is for -- without it, off would leave the
# candidates (and their Unknown chunks, rebuilt every frame) alive and on would wait for the
# camera to walk into another shell chunk.
#
# WHAT THIS DOES NOT PIN: that the near field stays CONSISTENT across a live toggle. It does
# not, and it cannot: brick and island lattices are baked where data is generated and are not
# re-baked on a toggle, so off leaves existing bricks marching as solids (a hole where the ice
# was) and on leaves them marching as air (the shell front lands at the same depth as the
# baked surface). enabled is a startup switch for that bake -- transparency_settings.h says so
# now -- and the A/B it exists for is applied before the world streams, which is what
# demo/benchmark.gd does in _ready.
func test_toggling_the_feature_off_and_on_drops_and_restores_the_shell() -> void:
	var w := shell_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	assert_int(int(frame_until_shell(w)["shell_pages"])).is_greater(0)
	w.set_transparency_value("enabled", 0.0)
	settle(w)
	var off := {}
	for i in range(120):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
		off = w.hooks().debug_lod_stats()
		if int(off["shell_pages"]) == 0:
			break
	assert_int(int(off["shell_pages"])).override_failure_message(
		"turning the feature off left the candidate set and its pages alive: %s" % off
		).is_equal(0)
	w.set_transparency_value("enabled", 1.0)
	var on := frame_until_shell(w)
	assert_int(int(on["shell_pages"])).override_failure_message(
		"turning the feature back on never rebuilt the shell: %s" % on).is_greater(0)

func test_with_the_feature_off_no_shell_is_built() -> void:
	var w := shell_world(false)
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	for i in range(60):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).is_equal(0)

# With the near field off the far field draws transparent solid itself, so there is no near
# shell to build. This passes with or without the near-field gate (the 1e9 m radius is
# refused by collect_ops_for_aabb's 128-region span cap, so the op list comes back empty
# anyway); what the gate buys is skipping that world-wide scan, and this pins the intent so a
# later span-cap change cannot quietly make it a world-wide candidate set.
func test_with_the_near_field_off_no_shell_is_built() -> void:
	var w := shell_world()
	w.set_effect_enabled("near_field", false)
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	for i in range(60):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).override_failure_message(
		"the near shell was built with no near field: %s" % w.hooks().debug_lod_stats()
		).is_equal(0)

# --- the shell raster (spec §6) -----------------------------------------------------------

# Frames until the shell raster has drawn a front at the centre pixel.
func frame_until_front(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if int(d["shell_pages"]) > 0 and (d["center_front"] as Color).a > 0.5:
			break
	return d

func test_the_front_layer_holds_the_ice_and_the_thickness_is_the_path_through_it() -> void:
	var w := shell_world()
	var ground := centre_hit(w)
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(ground["pos"], 1.0, ice)
	settle(w)
	var d := frame_until_front(w)
	var front: Color = d["center_front"]
	assert_int(int(front.a + 0.5)).override_failure_message(
		"no transparent front at the centre: %s" % d).is_equal(ice)
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("shell")).is_true()
	# The ball is centred on the ground: the ray enters its top and ends on the ground
	# inside it. One front, no back: G = 1, and thickness = R + G * z_ground ~ the radius.
	var t: Vector2 = d["center_thick"]
	assert_float(t.y).is_equal_approx(1.0, 0.01)
	var thickness: float = t.x + t.y * float(d["center_distance"])
	assert_float(thickness).override_failure_message(
		"thickness %f from %s at ground distance %s" % [thickness, t, d["center_distance"]]
		).is_between(0.6, 1.3)
	# The front is nearer than the ground by that thickness.
	assert_float(front.b).is_equal_approx(float(d["center_distance"]) - thickness, 0.05)

func test_a_floating_ball_has_a_matched_front_and_back() -> void:
	var w := shell_world()
	var ground := centre_hit(w)
	w.hooks().debug_apply_sphere_add(ground["pos"] + Vector3(0, 2.0, 0), 0.8, material_id(w, "ice"))
	settle(w)
	var d := frame_until_front(w)
	# A pass that silently stopped drawing leaves (0, 0), and t.y ~= 0 below would pass vacuously.
	assert_bool((d["center_front"] as Color).a > 0.5).override_failure_message(
		"the floating ball wrote no front: %s" % d).is_true()
	var t: Vector2 = d["center_thick"]
	assert_float(t.y).override_failure_message("fronts != backs: %s" % d).is_equal_approx(0.0, 0.01)
	# The centre ray passes near the ball's middle: about a diameter of ice.
	assert_float(t.x).is_between(1.0, 1.7)

# Far field: an ice ball ~200 m out, well past the fade band, is drawn by the LoD shell.
func test_far_ice_has_a_front_from_the_lod_shell() -> void:
	var w := shell_world()
	var down: Dictionary = w.raycast(Vector3(CAM.x + 200.0, 200.0, CAM.z), Vector3.DOWN, 300.0)
	assert_bool(down["hit"]).is_true()
	var target: Vector3 = down["pos"]
	var cam := Vector3(CAM.x, target.y + 60.0, CAM.z)
	var fwd := (target - cam).normalized()
	var seen: Dictionary = w.raycast(cam, fwd, 400.0)
	assert_bool(seen["hit"] and float(seen["distance"]) > 150.0).override_failure_message(
		"terrain hides the far ice from this camera; raise it").is_true()
	w.hooks().debug_apply_sphere_add(target, 10.0, material_id(w, "ice"))
	settle(w, cam)
	var d := frame_until_front(w, cam, fwd)
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"no far shell front: %s" % d).is_equal(material_id(w, "ice"))
	# No near-shell chunk exists out there: the page came from the far field.
	assert_int(int(w.hooks().debug_lod_stats()["shell_pages"])).is_equal(0)

# The lit-bit identity of an ice-free frame lives in
# test_a_scene_without_transparency_is_bit_identical_with_the_feature_on_and_off, which needs two
# worlds to compare; this one only pins the shell's own silence.
func test_a_frame_with_no_ice_draws_no_shell() -> void:
	var w := shell_world()
	var d := frame(w)
	assert_int(int(d["shell_pages"])).is_equal(0)
	assert_float((d["center_front"] as Color).a).is_equal(0.0)

# The gate in frame.cpp is skipped on a frame the feature is off, so the pass is never asked
# to draw -- but the targets still hold last frame's ice. drew() must go false with the gate,
# or the composite reads a front that is no longer there.
func test_toggling_transparency_off_stops_the_shell_reading_stale_targets() -> void:
	var w := shell_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, ice)
	settle(w)
	var on := frame_until_front(w)
	assert_bool((on["center_front"] as Color).a > 0.5).override_failure_message(
		"no front while the feature is on: %s" % on).is_true()
	w.set_transparency_value("enabled", 0.0)
	var off := frame(w)
	assert_int(int(off["shell_pages"])).is_equal(0)
	assert_bool((off["stages_ok"] as PackedStringArray).has("shell")).is_false()
	assert_float((off["center_front"] as Color).a).override_failure_message(
		"the shell read last frame's front: %s" % off).is_equal(0.0)
	assert_vector((off["center_thick"] as Vector2)).override_failure_message(
		"the shell read last frame's thickness: %s" % off).is_equal(Vector2.ZERO)
	# The shell-side asserts above all go through hooks.cpp's own drew() gate, so they would
	# pass even if the composite were still reading the targets -- this one cannot: the
	# composite only reports "transparency" when its render() dispatched.
	assert_bool((off["stages_ok"] as PackedStringArray).has("transparency")).override_failure_message(
		"the composite ran on last frame's shell targets: %s" % off).is_false()

# --- the composite (spec §6) --------------------------------------------------------------

func dist(a: Color, b: Color) -> float:
	return Vector3(a.r - b.r, a.g - b.g, a.b - b.b).length()

func test_ice_tints_the_ground_and_never_hides_it() -> void:
	var w := shell_world()
	var bare: Color = frame(w)["center_lit"]
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame_until_front(w)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("transparency")).is_true()
	assert_float(dist(lit, bare)).override_failure_message(
		"the ice left the pixel untouched: %s vs %s" % [lit, bare]).is_greater(0.01)
	# Blue survives ice better than red: the tint is the table's, not a grey fade.
	assert_float(lit.b / max(bare.b, 1e-4)).is_greater(lit.r / max(bare.r, 1e-4))

# THE regression test for the prior attempt: ten metres of ice is darker than one, and the
# ground behind it still contributes at least the floor.
# ONE world, ONE camera, three states: the ice grows in place, so bare/thin/thick are three
# pixels of one streamed world and the ordering comparison is world-local.
func test_ten_metres_of_ice_is_darker_than_one_but_still_shows_the_ground() -> void:
	var w := shell_world()
	var bare: Color = frame(w)["center_lit"]
	# The block the centre ray crosses, grown in place. The 1 m ball sits ON the ground: the
	# shell has a front but no back face there, so the measured path is front-to-opaque,
	# about a metre. The 5 m ball then swallows it (4 m up, tangent) and the same ray crosses
	# ~9 m of the material before reaching the ground.
	var ground: Vector3 = centre_hit(w)["pos"]
	w.hooks().debug_apply_sphere_add(ground, 1.0, material_id(w, "ice"))
	settle(w)
	var thin: Color = frame_until_front(w)["center_lit"]

	w.hooks().debug_apply_sphere_add(ground + Vector3(0, 4.0, 0), 5.0, material_id(w, "ice"))
	# NOT frame_until_front(): the 1 m ball's front is still sitting in the targets, so "there
	# is a front" goes true again the instant the edit lands and the helper returns the block
	# it replaced. Wait for the front to move NEARER instead -- the grown block's surface is
	# ~8 m in front of the small ball's, and nothing else writes this pixel.
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		var grown: Color = d["center_front"]
		if grown.a > 0.5 and grown.b < 5.0:
			break
	var thick: Color = d["center_lit"]
	var t: Vector2 = d["center_thick"]
	assert_float(t.x + t.y * float(d["center_distance"])).override_failure_message(
		"the path through the block is not ~9 m: %s" % d).is_greater(7.0)
	assert_float(dist(thick, bare)).override_failure_message(
		"thicker is not darker than bare: %s vs %s" % [thick, bare]).is_greater(dist(thin, bare))
	# Two frames, floor 0.05 then floor 0.9: with the floor raised more of the ground shows,
	# which is only possible if the ground behind the ice reached the pixel at all.
	w.set_transparency_value("min_transmit", 0.05)
	var low: Color = frame(w)["center_lit"]
	w.set_transparency_value("min_transmit", 0.9)
	var high: Color = frame(w)["center_lit"]
	assert_float(dist(high, low)).override_failure_message(
		"the transmittance floor changes nothing: the ground behind the ice is not in the pixel"
		).is_greater(0.02)

func test_sky_behind_a_floating_ice_ball_is_tinted_not_holed() -> void:
	var w := shell_world()
	var cam := CAM + Vector3(0, 30.0, 0)
	var fwd := Vector3(0.3, 1.0, 0.2) # up at the sky
	settle(w, cam)
	var sky: Color = frame(w, cam, fwd)["center_lit"]
	w.hooks().debug_apply_sphere_add(cam + fwd.normalized() * 6.0, 1.5, material_id(w, "ice"))
	settle(w, cam)
	var d := frame_until_front(w, cam, fwd)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(dist(lit, sky)).is_greater(0.005)
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05) # not a black hole

func test_a_camera_inside_ice_sees_a_tinted_world() -> void:
	var w := shell_world()
	var bare: Color = frame(w)["center_lit"]
	# A ball around the camera itself.
	w.hooks().debug_apply_sphere_add(CAM, 2.0, material_id(w, "ice"))
	settle(w)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int(d["shell_pages"]) > 0:
			break
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05)
	assert_float(dist(lit, bare)).override_failure_message(
		"two metres of ice around the camera tinted nothing").is_greater(0.01)
	# Only the back face is in view: G = +1 from the clear, -1 from the back = 0.
	assert_float((d["center_thick"] as Vector2).y).is_equal_approx(0.0, 0.01)
	# ...and no front was written, which is what puts this pixel on the !has_front branch
	# rather than the front-body branch. G == 0 alone would also fit a front/back pair.
	assert_float((d["center_front"] as Color).a).override_failure_message(
		"the camera-inside frame wrote a front: %s" % d).is_equal(0.0)

func test_ice_frames_are_finite_and_deterministic() -> void:
	var w := shell_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	frame_until_front(w)
	# Let the worker go quiet, then two back-to-back frames must be one image.
	for i in range(30):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	var a := frame(w)
	var b := frame(w)
	assert_int(int(a["lit_checksum"])).is_equal(int(b["lit_checksum"]))
	assert_bool(finite(a["center_lit"])).is_true()

# --- islands (spec §5, §7) ----------------------------------------------------------------

# An island lifted out of a patch of ground whose top 0.8 m is a transparent material, so it
# is a ROCK lump with an ice cap. That is the case the design rule exists for: the island's
# stored bytes are the SHARED authoritative volume (physics, merge back and the editor read
# them) and stay the union, so the surface the marcher must find -- the ice/rock boundary --
# is a LABEL boundary the union lattice has no min/max for. It is applied per sample, by
# island_lattice, only for islands whose descriptor asks for it; the shell is what draws the
# ice the marcher no longer sees.
func test_a_floating_island_with_an_ice_cap_is_see_through_and_its_shell_draws_the_ice() -> void:
	var w := shell_world()
	var p: Vector3 = centre_hit(w)["pos"]
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_paint(p, 0.8, ice)
	settle(w)
	# Exactly the occupancy cells (0.8 m) the painted lens touches. Derived from the lens, not
	# from one cell: a range that clipped it would leave the remainder painted in the terrain.
	var lo := Vector3i(floori((p.x - 0.8) / 0.8), floori((p.y - 0.8) / 0.8),
		floori((p.z - 0.8) / 0.8))
	var hi := Vector3i(floori((p.x + 0.8) / 0.8), floori((p.y + 0.8) / 0.8),
		floori((p.z + 0.8) / 0.8))
	var placed: Dictionary = w.hooks().debug_place_test_island(0, lo, hi, Vector3(0, 1.5, 0))
	assert_bool(placed.get("ok", false)).override_failure_message(str(placed)).is_true()
	# The fixture extracts the island but never carves the terrain -- the real pipeline carves
	# on the main thread -- so the painted lens is still lying in the ground, and a near-field
	# shell would draw THAT ice at the surface, in front of the island, which is a different
	# system entirely. Take it back out before the island is measured.
	w.hooks().debug_apply_sphere_subtract(p, 1.2)
	settle(w)
	var d := frame_until_front(w)
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"the island's ice cap has no shell front: %s" % d).is_equal(ice)
	# The marcher found the island's ROCK, not its ice: the opaque view erased the cap per
	# sample, so the ray crossed it and stopped on the lump under it. Without that rule the
	# first hit is the cap's own surface and this reads ice.
	assert_int(int(d["center_material"])).override_failure_message(
		"the marcher stopped on the island's own surface: %s" % d).is_not_equal(ice)
	assert_int(int(d["center_material"])).is_not_equal(0)
	assert_bool(finite(d["center_lit"])).is_true()
	# The island's shell pages count as OWNED: stats() sums island_shell_pages_ into
	# owned_pages, the same way shell_pages_of_ is summed. Without that sum the unowned-pages
	# term reports every island shell page as a leak, which silently disables the leak
	# detector the near-shell tests lean on -- in exactly the sessions that have transparent
	# islands. Checked here with the island live and its shell measurably drawing (the front
	# assertion above), so a zero is not read as "no pages, nothing to leak".
	var stats := w.hooks().debug_lod_stats()
	assert_int(int(stats["partial_allocations"])).override_failure_message(
		"the island's shell pages read as unowned: %s" % stats).is_equal(0)
	# Clearing the island releases its shell pages: nothing else in this world holds ice.
	w.hooks().debug_clear_test_island(0)
	for i in range(60):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
	assert_float((d["center_front"] as Color).a).override_failure_message(
		"the island's shell outlived it: %s" % d).is_equal(0.0)

# The same fixture with the camera PAST the fade band. This is the one that used to punch a
# hole rather than lose a tint: an island's shell pages carry chunk flag bit 0
# (ve::island_shell_flags), which the vertex shader reads as v_near == 1, so they take the
# NEAR side of the dither -- and past fade_end t_fade == 1 while bayer4 tops out at 15/16, so
# every fragment of the cap was discarded. Nothing filled the gap, because march_island is
# bounded by its own AABB and NOT by the camera's max_dist, and island_lattice applies the
# opaque view unconditionally: the marcher still drew the lump's opaque part at full opacity
# and saw AIR where the ice is. A solid island with a transparent hole in its cap.
#
# residency_radius_m = 40 puts the band's end at exactly 32 m (ve::lod_fade_band: min(150,
# floor(reach * 0.9 / 8) * 8) floored at kLodFadeMinEndM), so 38 m is past it by construction
# rather than by whatever reach this world happened to stream.
func test_an_island_past_the_fade_band_still_draws_its_ice_cap() -> void:
	var w := shell_world(true, 40.0)
	var p: Vector3 = centre_hit(w)["pos"]
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_paint(p, 0.8, ice)
	settle(w)
	# Exactly the occupancy cells (0.8 m) the painted lens touches, as in the fixture above.
	var lo := Vector3i(floori((p.x - 0.8) / 0.8), floori((p.y - 0.8) / 0.8),
		floori((p.z - 0.8) / 0.8))
	var hi := Vector3i(floori((p.x + 0.8) / 0.8), floori((p.y + 0.8) / 0.8),
		floori((p.z + 0.8) / 0.8))
	var placed: Dictionary = w.hooks().debug_place_test_island(0, lo, hi, Vector3(0, 1.5, 0))
	assert_bool(placed.get("ok", false)).override_failure_message(str(placed)).is_true()
	# The fixture never carves the terrain, so take the painted lens back out of the world
	# before the island is measured -- otherwise a near-field shell draws THAT ice instead.
	w.hooks().debug_apply_sphere_subtract(p, 1.2)
	settle(w)
	var centre: Vector3 = placed["world_center"]
	var cam := centre + Vector3(0.0, 38.0, 0.0)
	var fwd := Vector3(0.0, -1.0, 0.0)
	var d := frame_until_front(w, cam, fwd)
	# Past the band the FRAME reports, not a constant: the same seam the shader divides on.
	assert_float(cam.distance_to(centre)).override_failure_message(
		"the fixture camera is not past the band this frame used: %s" % d
		).is_greater(float(d["fade_end"]))
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"the island's ice cap has no shell front past the fade band -- the cap was dithered "
		+ "away and nothing is left in front of the island: %s" % d).is_equal(ice)
	# The marcher never stopped on the cap itself -- the opaque view erased it, so it found
	# the ground under the island.
	assert_int(int(d["center_material"])).override_failure_message(
		"the marcher stopped on the island's own surface: %s" % d).is_not_equal(ice)
	# ...and that opaque surface really is there, which is what makes the missing front a HOLE
	# rather than a missing tint: a 1x1 march from this exact camera and direction stops on
	# solid ground at the island's distance, so without the shell the pixel would show bare
	# ground where the cap is. (Probed, not read off the frame: the frame's raymarch runs at
	# render scale, and its centre texel is up to a texel off this axis.)
	var probe: Dictionary = w.hooks().debug_raymarch_probe(cam, fwd)
	assert_bool(probe["hit"]).override_failure_message(
		"nothing opaque at all behind the island from this camera: %s" % probe).is_true()
	assert_float(float(probe["pos"].distance_to(cam))).override_failure_message(
		"the opaque hit is not at the island's distance: %s" % probe
		).is_equal_approx(cam.distance_to(centre), 6.0)
	assert_int(int(probe["material"])).is_not_equal(ice)
	assert_bool(finite(d["center_lit"])).is_true()

# --- the G-buffer resolve (spec §6 step 5) ------------------------------------------------

# The finished scene colour as an Image: the hook hands back the half-float bytes of the
# frame's own scene colour attachment, which is exactly Image.FORMAT_RGBAH's layout.
func scene_image(d: Dictionary) -> Image:
	var bytes: PackedByteArray = d["scene_rgba"]
	assert_int(bytes.size()).override_failure_message(
		"no scene colour readout: %s" % d).is_equal(W * H * 8)
	return Image.create_from_data(W, H, false, Image.FORMAT_RGBAH, bytes)

func test_after_the_frame_the_gbuffer_holds_the_ice_front() -> void:
	var w := shell_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"], 1.0, ice)
	settle(w)
	var d := frame_until_front(w)
	assert_int(int(d["center_gb_material"])).override_failure_message(
		"outlines and SSR would still see the ground behind the ice: %s" % d).is_equal(ice)

# A floating ball is a depth break in the G-buffer the outline pass reads ONLY because the
# resolve put the ball's front there. Same scene, same world, two frames that differ in one
# setting: nothing else moves, so any pixel the outline pass darkened is its doing.
func test_the_ice_silhouette_gets_an_outline() -> void:
	var w := shell_world()
	# SSR carries its own history, which would differ between the pair and hide the outline's
	# signal in it. Contact shadows are a pure function of the frame and stay on.
	w.set_effect_enabled("ssr", false)
	w.hooks().debug_apply_sphere_add(centre_hit(w)["pos"] + Vector3(0, 2.0, 0), 0.6,
			material_id(w, "ice"))
	settle(w)
	w.set_effect_enabled("outlines", false)
	frame_until_front(w)
	var plain: Image = scene_image(frame(w))
	w.set_effect_enabled("outlines", true)
	var outlined: Image = scene_image(frame(w))
	var darkened := 0
	var row := H / 2
	for x in range(W):
		if plain.get_pixel(x, row).get_luminance() > 0.02 and \
				outlined.get_pixel(x, row).get_luminance() < \
				plain.get_pixel(x, row).get_luminance() * 0.5:
			darkened += 1
	assert_int(darkened).override_failure_message(
		"no pixel on the ball's row is outline-dark: the ball is not in the depth the outline "
		+ "pass reads").is_greater(0)
