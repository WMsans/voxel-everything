extends GdUnitTestSuite

# The headless frame's contract (docs/superpowers/specs/2026-09-13-frame-module-design.md §6
# Step 2). Everything here goes through debug_render_frame, which runs VoxelFrame -- the same
# object the compositors call -- on a local RenderingDevice. Headless is compared only with
# headless: it has no engine opaque objects.

const CAM := Vector3(30.0, 70.0, 30.0)
const FWD := Vector3(0.2, -1.0, 0.2)
const CORE := ["stream", "raymarch", "composite", "deferred", "inject", "history"]

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
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(CAM) == 0 else 0
		if quiet >= 6:
			break
	return w

func frame(w: VoxelWorld, width := 64, height := 64) -> Dictionary:
	return w.hooks().debug_render_frame(CAM, FWD.normalized(), width, height)

func test_resize_and_shutdown_release_gpu_resources_without_errors() -> void:
	var w := make_world()
	await assert_error(func():
		assert_bool(frame(w)["ok"]).is_true()
		assert_bool(frame(w, 96, 64)["ok"]).is_true()
		w.free()
	).is_success()

func test_a_headless_frame_runs_the_core_stages() -> void:
	var w := make_world()
	var d := frame(w)
	assert_bool(d["ok"]).override_failure_message("headless frame aborted: %s" % d).is_true()
	var ok: PackedStringArray = d["stages_ok"]
	var cancelled: PackedStringArray = d["stages_cancelled"]
	for stage in CORE:
		assert_bool(ok.has(stage)).override_failure_message(
			"stage %s did not complete: ok=%s cancelled=%s" % [stage, ok, cancelled]).is_true()
		assert_bool(cancelled.has(stage)).is_false()
	assert_float(float(d["mean_luma"])).override_failure_message(
		"the lit image is black; the camera saw nothing").is_greater(0.01)
	assert_float(float(d["fade_end"])).is_greater(float(d["fade_start"]))

# SSGI accumulates across frames and grass sways with the frame counter; with both held still
# the frame is a pure function of the world and the camera.
func test_two_frames_of_one_view_are_identical_without_temporal_effects() -> void:
	var w := make_world()
	w.set_effect_enabled("ssgi", false)
	w.set_grass_value("wind_strength", 0.0)
	var a := frame(w)
	var b := frame(w)
	assert_bool(a["ok"] and b["ok"]).is_true()
	assert_int(int(b["lit_checksum"])).override_failure_message(
		"two identical frames differ: %s vs %s" % [a["lit_checksum"], b["lit_checksum"]]
		).is_equal(int(a["lit_checksum"]))

# near_field_scale 0.66 of a 1x1 target is a 0x0 march: the frame must refuse softly, report
# which stages ran, and leave nothing behind that breaks the next frame.
func test_a_frame_too_small_to_march_aborts_softly() -> void:
	var w := make_world()
	var tiny := frame(w, 1, 1)
	assert_bool(tiny["ok"]).is_false()
	var ok: PackedStringArray = tiny["stages_ok"]
	assert_bool(ok.has("stream")).is_true()
	assert_bool(ok.has("raymarch")).is_false()
	var next := frame(w)
	assert_bool(next["ok"]).override_failure_message(
		"a normal frame after an aborted one failed: %s" % next).is_true()

func test_turning_the_near_field_off_skips_hiz() -> void:
	var w := make_world()
	assert_bool(w.hooks().debug_init_physics()).is_true()
	w.hooks().debug_lod_tick(CAM, FWD.normalized()) # creates the LoD pool the frame gates on
	var on := frame(w)
	assert_bool(on["hiz_built"]).override_failure_message(
		"near field on, but HiZ was not built: %s" % on).is_true()
	w.set_effect_enabled("near_field", false)
	var off := frame(w)
	assert_bool(off["hiz_built"]).is_false()
	assert_bool(off["two_phase"]).is_false()

func test_ssgi_history_carries_across_frames_and_falls_on_resize() -> void:
	var w := make_world()
	w.set_effect_enabled("ssgi", true)
	var first := frame(w)
	var second := frame(w)
	var resized := frame(w, 96, 64)
	assert_bool(first["had_history"]).is_false()
	assert_bool(second["had_history"]).override_failure_message(
		"the history written by frame 1 was not visible to frame 2").is_true()
	assert_bool(resized["had_history"]).override_failure_message(
		"a resized G-buffer still claimed last frame's history").is_false()
