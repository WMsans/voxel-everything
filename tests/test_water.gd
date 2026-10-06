extends GdUnitTestSuite

# Water shading, end to end (docs/superpowers/specs/2026-10-06-water-voxels-design.md §5,
# §6). Every frame is debug_render_frame -- the shipping VoxelFrame on a local device -- so
# what is asserted is what ships. Fixture conventions are tests/test_transparency.gd's: the
# sunlit meadow at (20, 60, 30), every temporal input held still, SSAO and SSGI off.

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

func make_world(enabled := true, cam := CAM) -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	w.set_transparency_value("enabled", 1.0 if enabled else 0.0)
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	w.set_effect_enabled("ssgi", false)
	w.set_effect_enabled("ssao", false)
	w.set_grass_value("enabled", 0.0)
	w.set_grass_value("wind_strength", 0.0)
	w.set_grass_value("wind_speed", 0.0)
	w.set_leaf_value("wind_strength", 0.0)
	w.set_leaf_value("wind_speed", 0.0)
	settle(w, cam)
	# The shell needs the MeshService (debug_init_physics) and the LoD pool (a LoD query):
	# test_transparency.gd's shell_world() preconditions.
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_stats()
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

func centre_hit(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var hit: Dictionary = w.raycast(cam, fwd.normalized(), 400.0)
	assert_bool(hit["hit"]).override_failure_message("the centre ray sees nothing").is_true()
	return hit

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), W, H)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

# The shell is built on the worker thread and lags an edit by a few frames.
func frame_until_front(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if (d["center_front"] as Color).a > 0.5:
			return d
	return d

func finite(c: Color) -> bool:
	return is_finite(c.r) and is_finite(c.g) and is_finite(c.b)

func dist(a: Color, b: Color) -> float:
	return Vector3(a.r - b.r, a.g - b.g, a.b - b.b).length()

func image_of(d: Dictionary) -> Image:
	return Image.create_from_data(W, H, false, Image.FORMAT_RGBAH, d["scene_rgba"])

# A shallow water cap: a 20 m ball whose top stands 0.4 m proud of the ground at `at`. The
# solid view keeps the ground under it (a liquid ADD contributes nothing), so the water is
# 0.4 m deep at the centre and thins to nothing at the rim.
func add_cap(w: VoxelWorld, at: Vector3) -> void:
	w.hooks().debug_apply_sphere_add(at - Vector3(0, 19.6, 0), 20.0, material_id(w, "water"))

# A pond: a 4 m crater under the same cap. The subtract carves ground and water alike; the
# cap re-added after it fills the crater, so the centre is ~4.4 m deep.
func add_pond(w: VoxelWorld, at: Vector3) -> void:
	w.hooks().debug_apply_sphere_subtract(at, 4.0)
	add_cap(w, at)

# --- §5: shading a front seen from above ---------------------------------------------------

func test_deep_water_is_closer_to_the_scatter_colour_than_shallow_water() -> void:
	var w := make_world()
	w.set_water_value("wave_strength", 0.0) # absorption alone, no glint to land on the pixel
	var ground: Vector3 = centre_hit(w)["pos"]
	var bare: Color = frame(w)["center_lit"]
	add_cap(w, ground)
	settle(w)
	var shallow_d := frame_until_front(w)
	assert_int(int((shallow_d["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "water"))
	var shallow: Color = shallow_d["center_lit"]
	add_pond(w, ground)
	settle(w)
	# Wait for the deepened pond's shell: the path through it grows from ~0.4 m to ~4.4 m.
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		var t: Vector2 = d["center_thick"]
		if t.x + t.y * float(d["center_distance"]) > 3.0:
			break
	var deep: Color = d["center_lit"]
	assert_bool(finite(shallow) and finite(deep)).is_true()
	assert_float(dist(shallow, bare)).override_failure_message(
		"0.4 m of water left the ground untouched").is_greater(0.005)
	assert_float(dist(deep, bare)).override_failure_message(
		"4 m of water is not further from bare ground than 0.4 m: %s %s %s" % [bare, shallow, deep]
		).is_greater(dist(shallow, bare))
	# Red dies first, and a liquid has no transmittance floor: 4 m leaves well under the 0.35
	# that ice would be floored at.
	assert_float(deep.r / max(bare.r, 1e-4)).is_less(0.35)
	assert_float(deep.b / max(bare.b, 1e-4)).is_greater(deep.r / max(bare.r, 1e-4))

func test_water_frames_change_over_time_and_the_ground_around_them_does_not() -> void:
	var w := make_world()
	add_pond(w, centre_hit(w)["pos"])
	settle(w)
	frame_until_front(w)
	# Let the worker go quiet, holding the world still with a render per stream frame (the
	# test_transparency.gd idiom): stream-only frames let the LoD/sun-shadow state settle
	# underneath, which moves ground pixels for reasons outside this test.
	for i in range(30):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	var a := frame(w)
	for i in range(30):
		w.hooks().debug_stream_frame(CAM)
		frame(w)
	var b := frame(w)
	assert_float(dist(a["center_lit"], b["center_lit"])).override_failure_message(
		"the water did not move in 30 frames").is_greater(1e-4)
	# A corner pixel is meadow well outside the 4 m pond: with every other input still, it
	# must be the same colour in both frames.
	var ia := image_of(a)
	var ib := image_of(b)
	var ca := ia.get_pixel(1, 1)
	var cb := ib.get_pixel(1, 1)
	assert_float(dist(ca, cb)).override_failure_message(
		"ground away from the water changed: %s vs %s" % [ca, cb]).is_less(1e-5)

# Refraction must never pull a foreground object into the water. The pillar is GLOWING
# (ground_crack_01): if its colour leaked into water pixels, the count of strongly orange
# pixels would grow when refraction is turned up.
func test_refraction_never_pulls_a_foreground_object_into_the_water() -> void:
	var w := make_world()
	var ground: Vector3 = centre_hit(w)["pos"]
	add_pond(w, ground)
	var mid := CAM.lerp(ground, 0.55) + Vector3(1.2, 0, 0)
	w.hooks().debug_apply_sphere_add(mid, 0.6, material_id(w, "ground_crack_01"))
	settle(w)
	frame_until_front(w)
	w.set_water_value("refraction_strength", 0.0)
	var still := image_of(frame(w))
	w.set_water_value("refraction_strength", 0.2)
	var bent := image_of(frame(w))
	var orange := func(img: Image) -> int:
		var n := 0
		for y in range(H):
			for x in range(W):
				var c := img.get_pixel(x, y)
				if c.r > 0.5 and c.r > 2.0 * c.b:
					n += 1
		return n
	var before: int = orange.call(still)
	var after: int = orange.call(bent)
	assert_int(before).override_failure_message("the glowing pillar is not in view").is_greater(0)
	assert_int(after).override_failure_message(
		"refraction pulled the pillar into the water: %d orange pixels, %d without" % [after, before]
		).is_less_equal(before + 2)

# Review Focus 1.
func test_water_against_the_sky_is_tinted_not_black() -> void:
	var w := make_world()
	var cam := CAM + Vector3(0, 30.0, 0)
	var fwd := Vector3(0.3, 1.0, 0.2)
	settle(w, cam)
	var sky: Color = frame(w, cam, fwd)["center_lit"]
	w.hooks().debug_apply_sphere_add(cam + fwd.normalized() * 6.0, 1.5, material_id(w, "water"))
	settle(w, cam)
	var d := frame_until_front(w, cam, fwd)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(dist(lit, sky)).is_greater(0.005)
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05)

# Review Focus 4.
func test_extreme_water_settings_stay_finite_and_deterministic() -> void:
	var w := make_world()
	add_pond(w, centre_hit(w)["pos"])
	settle(w)
	frame_until_front(w)
	for kv in [["wave_strength", 0.0], ["wave_strength", 3.0], ["refraction_strength", 0.2],
			["foam_width_m", 0.0], ["foam_width_m", 3.0], ["flow_speed", 5.0]]:
		w.set_water_value(kv[0], kv[1])
		var d := frame(w)
		assert_bool(finite(d["center_lit"])).override_failure_message(
			"%s=%s gave %s" % [kv[0], kv[1], d["center_lit"]]).is_true()
	# Still water (no flow, no waves) rendered twice with nothing else moving: the same image.
	w.set_water_value("flow_speed", 0.0)
	w.set_water_value("wave_strength", 0.0)
	w.set_water_value("foam_width_m", 0.0)
	var b1: int = int(frame(w)["lit_checksum"])
	var b2: int = int(frame(w)["lit_checksum"])
	assert_int(b2).override_failure_message("still water rendered twice differs").is_equal(b1)

# Review Focus 5.
func test_ice_floating_over_a_pond_keeps_its_own_front() -> void:
	var w := make_world()
	var ground: Vector3 = centre_hit(w)["pos"]
	add_pond(w, ground)
	w.hooks().debug_apply_sphere_add(ground + Vector3(0, 3.0, 0), 0.8, material_id(w, "ice"))
	settle(w)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int((d["center_front"] as Color).a + 0.5) == material_id(w, "ice"):
			break
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"the nearer ice is not the centre's front: %s" % d).is_equal(material_id(w, "ice"))
	assert_bool(finite(d["center_lit"])).is_true()

func test_the_shading_normal_reaches_the_front_the_gbuffer_resolves() -> void:
	var w := make_world()
	add_pond(w, centre_hit(w)["pos"])
	settle(w)
	frame_until_front(w)
	w.set_water_value("wave_strength", 0.0)
	var flat: Color = frame(w)["center_front"]
	w.set_water_value("wave_strength", 3.0)
	var rippled: Color = frame(w)["center_front"]
	# xy is the octahedral normal the resolve copies into the G-buffer; z (distance) and w
	# (material) are the front pass's and do not move.
	assert_float(Vector2(flat.r - rippled.r, flat.g - rippled.g).length()).is_greater(1e-3)
	assert_float(flat.b).is_equal_approx(rippled.b, 1e-4)
	assert_float(flat.a).is_equal(rippled.a)

func test_with_transparency_off_water_draws_as_an_opaque_surface() -> void:
	var w := make_world(false)
	add_cap(w, centre_hit(w)["pos"])
	settle(w)
	var d := frame(w)
	assert_int(int(d["center_material"])).is_equal(material_id(w, "water"))
	assert_float((d["center_front"] as Color).a).is_equal(0.0)
	assert_bool(finite(d["center_lit"])).is_true()

# --- §6: the underwater view ---------------------------------------------------------------

func luma(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b

func frame_with_pages(w: VoxelWorld, cam: Vector3, fwd: Vector3) -> Dictionary:
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		if int(d["shell_pages"]) > 0:
			break
	return d

func test_a_camera_inside_water_sees_fog_not_black() -> void:
	var w := make_world()
	var bare: Color = frame(w)["center_lit"]
	w.hooks().debug_apply_sphere_add(CAM, 3.0, material_id(w, "water"))
	settle(w)
	var d := frame_with_pages(w, CAM, FWD)
	var lit: Color = d["center_lit"]
	assert_bool(finite(lit)).is_true()
	assert_float(lit.r + lit.g + lit.b).is_greater(0.05)
	assert_float(dist(lit, bare)).override_failure_message(
		"three metres of water around the camera changed nothing").is_greater(0.01)

func test_the_surface_above_an_underwater_camera_is_a_front() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(CAM, 3.0, material_id(w, "water"))
	settle(w)
	var up := Vector3(0.1, 1.0, 0.1)
	var d := frame_with_pages(w, CAM, up)
	# Seen from inside, the surface overhead is a BACK face; the exit-face pipeline records it.
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"no front for the surface overhead: %s" % d).is_equal(material_id(w, "water"))
	assert_float((d["center_front"] as Color).b).is_between(2.0, 4.0)

# A big ball whose top sits 1 m above the camera: locally an almost flat surface overhead.
# Straight up is inside Snell's window (sky through the surface); 15 degrees above the
# horizon is past the critical angle (the water's own dark body colour).
func test_snells_window_is_brighter_than_total_internal_reflection() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_add(CAM - Vector3(0, 19.0, 0), 20.0, material_id(w, "water"))
	settle(w)
	var up := frame_with_pages(w, CAM, Vector3(0.02, 1.0, 0.0))
	var graze := frame(w, CAM, Vector3(1.0, 0.27, 0.0))
	assert_int(int((up["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "water"))
	assert_int(int((graze["center_front"] as Color).a + 0.5)).is_equal(material_id(w, "water"))
	assert_float(luma(up["center_lit"])).override_failure_message(
		"straight up %s is not brighter than grazing %s" % [up["center_lit"], graze["center_lit"]]
		).is_greater(luma(graze["center_lit"]))

# Review Focus 3: a camera a few centimetres under a small ball's top, nudged up and down
# through the surface, flips `inside` frame to frame. Every frame must stay finite and lit.
func test_a_camera_crossing_a_small_surface_stays_finite() -> void:
	var w := make_world()
	var c := CAM - Vector3(0, 0.55, 0)
	w.hooks().debug_apply_sphere_add(c, 0.6, material_id(w, "water"))
	settle(w)
	frame_with_pages(w, CAM, FWD)
	for dy in [-0.08, -0.03, 0.02, 0.07, -0.05]:
		var d := frame(w, CAM + Vector3(0, dy, 0), FWD)
		var lit: Color = d["center_lit"]
		assert_bool(finite(lit)).override_failure_message("dy=%s gave %s" % [dy, lit]).is_true()
		assert_float(lit.r + lit.g + lit.b).is_greater(0.02)
