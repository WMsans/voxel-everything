extends GdUnitTestSuite

# Medium measured identical to High at the target config because SSAO -- 37% of the frame --
# had no knob for a tier to turn. A tier that cannot move the largest cost is not a tier.

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
	return w

func test_medium_costs_less_ssao_work_than_high() -> void:
	var w := make_world()
	w.set_quality_tier(3)
	var high: Dictionary = w.hooks().debug_beauty_settings()
	w.set_quality_tier(2)
	var med: Dictionary = w.hooks().debug_beauty_settings()
	var high_work: int = int(high["ssao_steps"]) * int(high["ssao_directions"])
	var med_work: int = int(med["ssao_steps"]) * int(med["ssao_directions"])
	assert_int(med_work).override_failure_message(
		"Medium asks for the same SSAO work as High (%d samples/pixel) -- the tier is a no-op"
		% med_work).is_less(high_work)

func test_zero_ssao_work_disables_the_pass() -> void:
	var w := make_world()
	w.set_quality_tier(1)
	var low: Dictionary = w.hooks().debug_beauty_settings()
	# Low turns SSAO off outright; the clamp must agree rather than leaving a dispatch that
	# produces nothing. Same rule the ssgi_taps == 0 case already follows.
	assert_bool(low["ssao"]).is_false()
