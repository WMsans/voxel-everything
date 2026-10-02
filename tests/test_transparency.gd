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
	# Hold every temporal input still, so two frames of one view are one image. SSAO, grass
	# and leaves are in that list because each one is NONDETERMINISTIC in the image (SSAO's
	# rotation and grass's scatter order differ between two worlds that never touched each
	# other), which would make the bit-identical pin below a coin flip. Nothing asserted here
	# needs any of the three.
	w.set_effect_enabled("ssgi", false)
	w.set_effect_enabled("ssao", false)
	w.set_effect_enabled("leaves", false)
	w.set_grass_value("enabled", 0.0)
	w.set_grass_value("wind_strength", 0.0)
	w.set_grass_value("wind_speed", 0.0)
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
	# The ball's own surface would be ~1 m NEARER than the bare ground (the opaque build
	# measures 10.28 against 11.33). The hit moved the other way, so the ray went through the
	# ice and stopped on the ground under it -- and only a little past the bare surface, so it
	# is that ground and not the ball's underside several metres down.
	var dist := float(d["center_distance"])
	assert_float(dist).override_failure_message(
		"the ray still stops at the ice's own surface, which is nearer: %s" % d).is_greater(
		float(before["center_distance"]) + 0.1)
	assert_float(dist).override_failure_message(
		"the ray marched far past the ground the ice is sitting on: %s" % d).is_less(
		float(before["center_distance"]) + 1.5)

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
func test_painted_ice_exposes_the_bowl_under_the_lens() -> void:
	var w := make_world()
	var ground := centre_hit(w)
	var before := frame(w)
	w.hooks().debug_apply_sphere_paint(ground["pos"], 1.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).is_not_equal(material_id(w, "ice"))
	assert_int(int(d["center_material"])).is_not_equal(0)
	# The bowl's bottom is about one radius further along the ray.
	assert_float(float(d["center_distance"])).is_greater(float(before["center_distance"]) + 0.5)
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