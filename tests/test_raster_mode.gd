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