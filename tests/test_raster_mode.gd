extends GdUnitTestSuite

# Raster mode (docs/superpowers/specs/2026-10-04-raster-mode-design.md): with the Raymarching
# switch off every surface is a mesh. Frames go through debug_render_frame, the shipping
# VoxelFrame. LoD LEVELS are settled through debug_lod_tick at 2560x1440: a 64x64 probe frame
# selects the coarse cut its own viewport asks for.

const CAM := Vector3(30.0, 70.0, 30.0)
const FWD := Vector3(0.2, -1.0, 0.2)

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
	assert_bool(w.hooks().debug_init_physics()).is_true()
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(CAM) == 0 else 0
		if quiet >= 6:
			break
	w.hooks().debug_lod_tick(CAM, FWD.normalized()) # creates the LoD pool the frame gates on
	return w

func frame(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d: Dictionary = w.hooks().debug_render_frame(cam, fwd.normalized(), 64, 64)
	assert_bool(d["ok"]).override_failure_message("frame aborted: %s" % d).is_true()
	return d

func lod_quiet(w: VoxelWorld) -> bool:
	var s: Dictionary = w.hooks().debug_lod_stats()
	return int(s["requests_pending"]) == 0 and int(s["builds_in_flight"]) == 0

# Frames until the walk this 64x64 frame asks for has nothing left to build.
func settle_frames(w: VoxelWorld, cam := CAM, fwd := FWD) -> Dictionary:
	var d := {}
	var quiet := 0
	for i in range(600):
		w.hooks().debug_stream_frame(cam)
		d = frame(w, cam, fwd)
		quiet = quiet + 1 if lod_quiet(w) else 0
		if quiet >= 4:
			break
	return d

# Ticks at 2560x1440 until the walk has nothing left to build.
func settle_ticks(w: VoxelWorld, cam := CAM, fwd := FWD) -> void:
	var quiet := 0
	for i in range(2000):
		w.hooks().debug_stream_frame(cam)
		w.hooks().debug_lod_tick(cam, fwd.normalized())
		quiet = quiet + 1 if lod_quiet(w) else 0
		if quiet >= 4:
			break

# With the near field off the far field owns every pixel. It used to own one in sixteen:
# fade_band() reported 0 / 1e9, lod.frag.glsl keeps a fragment where bayer4 < d / 1e9, and
# only the bayer4 == 0 pixel of each 4x4 tile clears that.
func test_with_the_near_field_off_the_far_field_covers_the_ground() -> void:
	var w := make_world()
	w.set_effect_enabled("near_field", false)
	var d := settle_frames(w)
	assert_float(float(d["gb_ground_fraction"])).override_failure_message(
		"the far field covered %s of a view of nothing but ground" % d["gb_ground_fraction"]
		).is_greater(0.95)

func test_raster_mode_refines_the_near_field_to_ten_centimetres() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	settle_ticks(w)
	var s: Dictionary = w.hooks().debug_lod_stats()
	assert_int(int(s["draw_min_level"])).override_failure_message(str(s)).is_equal(-2)
	assert_int(int(s["partial_allocations"])).is_equal(0)

func test_raymarched_mode_never_draws_below_level_zero() -> void:
	var w := make_world()
	settle_ticks(w)
	var s: Dictionary = w.hooks().debug_lod_stats()
	assert_int(int(s["draw_min_level"])).override_failure_message(str(s)).is_greater_equal(0)
	assert_int(int(s["fine_pages"])).is_equal(0)

# Review focus 1: a live switch back must give the fine pages back. They are not freed
# eagerly; they age out like any chunk the walk stops touching (kLodEvictFrames).
func test_switching_back_to_raymarching_frees_the_fine_pages() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	settle_ticks(w)
	assert_int(int(w.hooks().debug_lod_stats()["fine_pages"])).is_greater(0)
	w.set_effect_enabled("raymarch", true)
	var s := {}
	for i in range(2000):
		w.hooks().debug_lod_tick(CAM, FWD.normalized())
		s = w.hooks().debug_lod_stats()
		if int(s["fine_pages"]) == 0 and int(s["builds_in_flight"]) == 0:
			break
	assert_int(int(s["fine_pages"])).override_failure_message(str(s)).is_equal(0)
	assert_int(int(s["partial_allocations"])).is_equal(0)

# Review focus 3: an edit in raster mode must reach the 0.1 m chunks, not stop at level 0.
func test_an_edit_in_raster_mode_requests_the_fine_chunks() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	settle_ticks(w)
	var hit: Dictionary = w.raycast(CAM, FWD.normalized(), 400.0)
	assert_bool(hit["hit"]).is_true()
	w.hooks().debug_apply_sphere_subtract(hit["pos"], 2.0)
	w.hooks().debug_lod_tick(CAM, FWD.normalized())
	var ids: Array = w.hooks().debug_lod_stats()["pending_request_ids"]
	var fine := ids.filter(func(id): return String(id).begins_with("-2:"))
	assert_int(fine.size()).override_failure_message(
		"the crater requested no 0.1 m rebuild: %s" % [ids]).is_greater(0)
	settle_ticks(w)
	assert_int(int(w.hooks().debug_lod_stats()["dirty_chunks"])).is_equal(0)
func material_id(w: VoxelWorld, name: String) -> int:
	for m in w.material_table():
		if m["name"] == name:
			return m["id"]
	return 0

func test_a_raster_frame_runs_without_the_marcher() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	var d := settle_frames(w)
	var ok: PackedStringArray = d["stages_ok"]
	for stage in ["stream", "composite", "lod", "deferred", "inject"]:
		assert_bool(ok.has(stage)).override_failure_message(
			"stage %s did not complete: %s" % [stage, ok]).is_true()
	assert_bool(ok.has("raymarch")).override_failure_message(
		"the marcher ran in raster mode: %s" % [ok]).is_false()
	assert_float(float(d["gb_ground_fraction"])).is_greater(0.95)
	assert_float(float(d["mean_luma"])).override_failure_message(
		"the lit image is black").is_greater(0.01)

# Review focus 4: nothing draws the sky in raster mode except the composite's sky fill.
func test_looking_up_in_raster_mode_shows_the_sky() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	var up := Vector3(0.2, 1.0, 0.2)
	var d := settle_frames(w, CAM, up)
	assert_float(float(d["gb_ground_fraction"])).is_less(0.05)
	var c: Color = d["center_lit"]
	assert_bool(c.b > c.r and c.b > 0.05).override_failure_message(
		"the centre is not sky: %s" % c).is_true()

# Review focus 5: the near shell grid is off in raster mode, so the fine LoD chunks' own
# shell quads are what draw ice.
func test_ice_renders_in_raster_mode() -> void:
	var w := make_world()
	w.set_effect_enabled("raymarch", false)
	var ice := material_id(w, "ice")
	var hit: Dictionary = w.raycast(CAM, FWD.normalized(), 400.0)
	assert_bool(hit["hit"]).is_true()
	w.hooks().debug_apply_sphere_add(hit["pos"], 1.0, ice)
	var d := {}
	for i in range(600):
		w.hooks().debug_stream_frame(CAM)
		d = frame(w)
		if int((d["center_front"] as Color).a + 0.5) == ice:
			break
	assert_int(int((d["center_front"] as Color).a + 0.5)).override_failure_message(
		"no ice front in raster mode: %s" % d).is_equal(ice)
