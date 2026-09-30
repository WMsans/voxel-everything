extends GdUnitTestSuite

# Transparent materials, end to end (docs/superpowers/specs/2026-09-29-transparent-materials-
# design.md §9). Every frame is debug_render_frame -- the shipping VoxelFrame on a local
# device -- so what is asserted is what ships, never a hook's re-implementation of it.
#
# (20, 60, 30) is the open, sunlit meadow (memory: grass-hook-spot-sees-cave-grass); the camera
# sits above it looking almost straight down, so the centre pixel is lit ground.

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

func make_world(cam := CAM) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	# Hold every temporal input still, so two frames of one view are one image.
	w.set_effect_enabled("ssgi", false)
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

# Where the centre pixel's ray first meets the world.
func centre_hit(w: VoxelWorld, cam := CAM, fwd := FWD) -> Vector3:
	var hit: Dictionary = w.raycast(cam, fwd.normalized(), 400.0)
	assert_bool(hit["hit"]).override_failure_message("the centre ray sees nothing").is_true()
	return hit["pos"]

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), W, H)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

func finite(c: Color) -> bool:
	return is_finite(c.r) and is_finite(c.g) and is_finite(c.b)

# --- the marcher (spec §4) --------------------------------------------------------------

func test_the_marcher_sees_through_ice_to_the_ground() -> void:
	var w := make_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, ice)
	settle(w)
	var d := frame(w)
	var front: Color = d["center_front"]
	var t: Color = d["center_trans"]
	assert_int(int(front.a + 0.5)).override_failure_message(
		"no transparent front at the centre: %s" % d).is_equal(ice)
	assert_int(int(d["center_material"])).override_failure_message(
		"the G-buffer holds the ice, not what is behind it").is_not_equal(ice)
	assert_float(t.r).is_greater(0.0)
	assert_float(t.r).is_less(1.0)
	# Blue survives ice better than red: the tint is the table's, not a grey fade.
	assert_float(t.b).is_greater(t.r)

func test_thicker_ice_lets_less_through() -> void:
	var thin_w := make_world()
	thin_w.hooks().debug_apply_sphere_add(centre_hit(thin_w), 0.6, material_id(thin_w, "ice"))
	settle(thin_w)
	var thin: Color = frame(thin_w)["center_trans"]
	var thick_w := make_world()
	thick_w.hooks().debug_apply_sphere_add(centre_hit(thick_w), 1.6, material_id(thick_w, "ice"))
	settle(thick_w)
	var thick: Color = frame(thick_w)["center_trans"]
	assert_float(thick.r).override_failure_message(
		"thin %s vs thick %s" % [thin, thick]).is_less(thin.r)
	assert_float(thick.r).is_greater(0.0)

func test_the_ground_under_ice_is_lit_through_it() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, material_id(w, "ice"))
	settle(w)
	# The sun visibility the G-buffer carries is the SURFACE BEHIND's: its sun ray crosses
	# the ice, and an opaque march would return 0 here.
	assert_float(float(frame(w)["center_sun"])).is_greater(0.3)

# ice_crack keeps at most 70% per metre, so a 0.5 cutoff fires after ~2 m -- well inside a
# 1.5 m ball's ~2.3 m path and well before the 48-step cap (48 x 5 cm = 2.4 m) could.
func test_past_the_cutoff_the_ice_is_opaque_and_the_gbuffer_keeps_it() -> void:
	var w := make_world()
	var ice := material_id(w, "ice_crack")
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.5, ice)
	settle(w)
	w.set_transparency_value("min_transmit", 0.5)
	var d := frame(w)
	var t: Color = d["center_trans"]
	assert_float(t.r + t.g + t.b).is_equal(0.0)
	assert_int(int(d["center_material"])).is_equal(ice)

func test_turning_transparency_off_hits_ice_as_opaque() -> void:
	var w := make_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.0, ice)
	settle(w)
	w.set_transparency_value("enabled", 0.0)
	var d := frame(w)
	assert_float(float((d["center_front"] as Color).a)).is_equal(0.0)
	assert_int(int(d["center_material"])).is_equal(ice)

# --- review focus ---------------------------------------------------------------------

func test_a_camera_inside_ice_still_sees_through_it() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(CAM, 2.0, material_id(w, "ice"))
	settle(w)
	var d := frame(w)
	assert_bool(finite(d["center_lit"])).is_true()
	assert_float((d["center_trans"] as Color).r).is_greater(0.0)

func test_extreme_settings_stay_finite() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(centre_hit(w), 1.5, material_id(w, "ice"))
	settle(w)
	w.set_transparency_value("max_steps", 1.0)
	w.set_transparency_value("min_transmit", 0.0)
	var capped := frame(w)
	var t: Color = capped["center_trans"]
	assert_float(t.r + t.g + t.b).override_failure_message(
		"one step cannot cross 3 m of ice; the cap must absorb").is_equal(0.0)
	w.set_transparency_value("max_steps", 48.0)
	w.set_transparency_value("min_step_m", 1.0)
	var coarse := frame(w)
	assert_bool(finite(coarse["center_lit"])).is_true()
	assert_bool(finite(coarse["center_trans"])).is_true()

func test_sky_behind_a_floating_ice_ball_shows_through() -> void:
	var w := make_world()
	# A ball in the air straight ahead of a level camera, with nothing behind it but sky.
	var fwd := Vector3(1.0, 0.25, 0.0)
	var ball := CAM + fwd.normalized() * 8.0
	w.hooks().debug_apply_sphere_add(ball, 1.5, material_id(w, "ice"))
	settle(w)
	var d := frame(w, CAM, fwd)
	assert_int(int((d["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "ice"))
	assert_int(int(d["center_material"])).override_failure_message(
		"behind the ball should be sky (material 0)").is_equal(0)
	assert_float((d["center_trans"] as Color).r).is_greater(0.0)

func test_painted_ice_shows_the_ground_below_the_lens() -> void:
	var w := make_world()
	var ice := material_id(w, "ice")
	w.hooks().debug_apply_sphere_paint(centre_hit(w), 1.0, ice)
	settle(w)
	var d := frame(w)
	assert_int(int((d["center_front"] as Color).a + 0.5)).is_equal(ice)
	assert_int(int(d["center_material"])).is_not_equal(ice)
	assert_float((d["center_trans"] as Color).r).is_greater(0.0)

# --- the far field (spec §5) -------------------------------------------------------------

# Frames until the LoD has built and published the shell, or gives up. Builds are async on
# the mesh worker; each frame collects what finished.
func frame_until_shell(w: VoxelWorld, cam: Vector3, fwd: Vector3) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if int(d.get("transparent_pages", 0)) > 0:
			break
	return d

func far_setup(w: VoxelWorld) -> Array:
	# The far field's LoD needs two things a near-field-only world never asks for, and both
	# are what every other LoD suite in this repo already does. The builds run on the
	# MeshService that debug_init_physics() creates, and the frame's far-field block is
	# gated on the LoD pool, which only a LoD query creates. Without either, the walk asks
	# for 32 chunks and nothing ever builds them: transparent_pages stays 0 forever.
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_stats()
	# An ice ball on the ground ~200 m out: well past the near field's fade band.
	var down: Dictionary = w.raycast(Vector3(CAM.x + 200.0, 200.0, CAM.z), Vector3.DOWN, 300.0)
	assert_bool(down["hit"]).is_true()
	var target: Vector3 = down["pos"]
	var cam := Vector3(CAM.x, target.y + 60.0, CAM.z)
	var fwd := (target - cam).normalized()
	var seen: Dictionary = w.raycast(cam, fwd, 400.0)
	assert_bool(seen["hit"] and float(seen["distance"]) > 150.0).override_failure_message(
		"terrain hides the far ice from this camera; raise it").is_true()
	return [cam, fwd, target]

func test_far_ice_is_drawn_by_the_shell() -> void:
	var w := make_world()
	var s := far_setup(w)
	w.hooks().debug_apply_sphere_add(s[2], 10.0, material_id(w, "ice"))
	settle(w, s[0])
	var d := frame_until_shell(w, s[0], s[1])
	assert_int(int(d["transparent_pages"])).override_failure_message(
		"no shell pages were ever published: %s" % d).is_greater(0)
	var ok: PackedStringArray = d["stages_ok"]
	assert_bool(ok.has("transparent_raster")).is_true()
	assert_int(int((d["far_center_front"] as Color).a + 0.5)).is_equal(material_id(w, "ice"))

func test_removing_far_ice_drops_its_shell() -> void:
	var w := make_world()
	var s := far_setup(w)
	w.hooks().debug_apply_sphere_add(s[2], 10.0, material_id(w, "ice"))
	settle(w, s[0])
	assert_int(int(frame_until_shell(w, s[0], s[1])["transparent_pages"])).is_greater(0)
	w.hooks().debug_apply_sphere_subtract(s[2], 12.0)
	settle(w, s[0])
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(s[0])
		d = frame(w, s[0], s[1])
		if int(d["transparent_pages"]) == 0:
			break
	assert_int(int(d["transparent_pages"])).override_failure_message(
		"the shell outlived the ice: released pages were never forgotten").is_equal(0)
