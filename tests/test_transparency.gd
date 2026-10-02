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
func make_world(enabled := true, cam := CAM) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
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
func shell_world(enabled := true) -> VoxelWorld:
	var w := make_world(enabled)
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
		"turning the feature off left the shell built: %s" % off).is_equal(0)
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
		if int(d.get("shell_pages", 0)) > 0 and (d["center_front"] as Color).a > 0.5:
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

func test_a_frame_with_no_ice_draws_no_shell_and_is_unchanged() -> void:
	var w := shell_world()
	var d := frame(w)
	assert_int(int(d["shell_pages"])).is_equal(0)
	assert_float((d["center_front"] as Color).a).is_equal(0.0)
