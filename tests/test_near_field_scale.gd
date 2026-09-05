extends GdUnitTestSuite

# near_field_scale trades MARCH resolution for frame time: the raymarcher visits fewer pixels
# and the composite magnifies its G-buffer to the internal size. Visibility is what gets
# cheaper -- which surface a pixel sees, and where. The material on that surface is a pure
# function of the hit position, the normal and the material id, all three of which the
# marcher exports per pixel, so it can be resolved once per FULL-resolution pixel for the
# same answer the scale-1.0 frame would have given.
#
# These tests hold that line: dropping the march scale must not drop the texture detail in
# the composited albedo. Before the material resolve moved into composite.frag.glsl the
# marcher baked its triplanar fetch at the march resolution -- with ray differentials sized
# to a low-resolution pixel, so the mip was over-selected too -- and the composite could only
# magnify the result. Detail then fell roughly in proportion to the scale, which is the
# blurring this suite fails on.

const CAM := Vector3(20.0, 70.0, 20.0)
const DOWN := Vector3(0, -1, 0)
const W := 256
const H := 144

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
	return w

func detail_at(w: VoxelWorld, scale: float) -> Dictionary:
	var d: Dictionary = w.hooks().debug_near_field_detail(CAM, DOWN, W, H, scale)
	assert_bool(d["ran"]).override_failure_message(
		"the near-field probe did not run at scale %.2f" % scale).is_true()
	assert_int(d["hit_pixels"]).override_failure_message(
		"scale %.2f: the camera saw no terrain, so there is no texture to measure" % scale
		).is_greater(W * H / 4)
	return d

# The fixture itself: at full scale the ground must carry real texture, or every ratio below
# would be a ratio of two zeroes.
func test_the_full_scale_near_field_has_texture_detail() -> void:
	var w := make_world()
	var full := detail_at(w, 1.0)
	assert_float(full["detail"]).override_failure_message(
		"a full-scale near field shaded flat: no material texture is reaching the G-buffer"
		).is_greater(0.004)

# THE regression. 0.40 is the shipped demo's near_field_scale (demo/main.tscn).
func test_a_low_march_scale_keeps_the_texture_detail() -> void:
	var w := make_world()
	var full := detail_at(w, 1.0)
	var low := detail_at(w, 0.4)
	assert_int(low["march_width"]).is_equal(int(W * 0.4))
	var ratio: float = float(low["detail"]) / maxf(float(full["detail"]), 1e-6)
	assert_float(ratio).override_failure_message(
		"marching at 0.40 kept only %.0f%% of the texture detail (%.4f vs %.4f): the material is being resolved at the march resolution and magnified, not resolved per full-resolution pixel"
		% [ratio * 100.0, low["detail"], full["detail"]]
		).is_greater(0.8)

# Half scale sits between the two and must not sag either: the relationship has to be flat
# across the slider's range, not merely repaired at one point.
func test_the_detail_is_flat_across_the_scale_range() -> void:
	var w := make_world()
	var full := detail_at(w, 1.0)
	for scale in [0.75, 0.5, 0.25]:
		var d := detail_at(w, scale)
		var ratio: float = float(d["detail"]) / maxf(float(full["detail"]), 1e-6)
		assert_float(ratio).override_failure_message(
			"scale %.2f kept only %.0f%% of the texture detail" % [scale, ratio * 100.0]
			).is_greater(0.75)

# Detail is not the same as noise: a correct resolve keeps the average colour where it was.
# A ratio test alone would pass on a shader that multiplied the albedo by white noise.
func test_the_mean_albedo_does_not_move_with_the_scale() -> void:
	var w := make_world()
	var full := detail_at(w, 1.0)
	var low := detail_at(w, 0.4)
	assert_float(float(low["mean_luma"])).override_failure_message(
		"the mean albedo moved from %.4f to %.4f when the march scale changed"
		% [full["mean_luma"], low["mean_luma"]]
		).is_equal_approx(float(full["mean_luma"]), 0.03)
