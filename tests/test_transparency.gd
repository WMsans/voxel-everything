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
