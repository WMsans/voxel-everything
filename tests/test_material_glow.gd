extends GdUnitTestSuite

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
	settle(w)
	return w

# Stream until the world goes quiet, FROM THE EDIT'S OWN CENTRE. A paint marks bricks dirty
# and they have to regenerate before anything can see the new material. debug_deferred_probe
# streams too, but from the CAMERA -- 19 m above the surface here -- and that loop exits as
# soon as it goes quiet, which it does before the repainted bricks near the ground are
# rebuilt. Painting and probing without this in between measured the terrain the paint had
# not reached yet: every assertion below then compared a surface to itself.
func settle(w: VoxelWorld) -> void:
	var quiet := 0
	for i in range(400):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(Vector3(20.0, 56.2, 20.0)) == 0 else 0
		if quiet >= 6:
			break

func magma_id() -> int:
	for m in _worlds[0].material_table():
		if float(m["glow"]) > 0.0:
			return int(m["id"])
	return -1

# Paint a patch of terrain with the emissive material and look at it. The lit buffer is
# rgba16f, so the emissive term is visible as HDR above what the same geometry reads
# with a non-emissive material -- which is exactly what Godot's bloom keys off.
func test_an_emissive_material_is_brighter_than_a_dull_one() -> void:
	var w := make_world()
	var id := magma_id()
	assert_int(id).override_failure_message(
		"no material in the table has a non-zero glow").is_greater(0)
	var pos := Vector3(20.0, 75.0, 20.0)
	var down := Vector3(0, -1, 0)

	# NOTE: the plan draft used radius 6.0 at this centre, but the terrain surface here
	# sits at y ~= 49.6, so that sphere floated above the ground and painted nothing; it
	# also covered too little of the probe frame to move mean_luma. Radius 16 both reaches
	# the surface and dominates the view, which is what these assertions actually need.
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 16.0, 1) # dull
	settle(w)
	var dull: Dictionary = w.hooks().debug_deferred_probe(pos, down, 64, 64, 0)
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 16.0, id) # emissive
	settle(w)
	var lit: Dictionary = w.hooks().debug_deferred_probe(pos, down, 64, 64, 0)

	assert_float(lit["mean_luma"]).override_failure_message(
		"the emissive material shaded no brighter than the dull one: glow is not applied"
		).is_greater(float(dull["mean_luma"]) * 1.5)

# Emission must survive as HDR. If it were clamped to 1.0 the term would still "work" in
# the lit buffer and then contribute nothing at all to Godot's glow, which thresholds above 1.
func test_emission_pushes_the_lit_buffer_above_one() -> void:
	var w := make_world()
	var id := magma_id()
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 16.0, id)
	settle(w)
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	var c: Color = d["center"]
	assert_float(maxf(c.r, maxf(c.g, c.b))).override_failure_message(
		"emissive pixel peaked at %.3f: nothing will bloom" % maxf(c.r, maxf(c.g, c.b))
		).is_greater(1.0)

# A non-emissive material must pay nothing and change nothing.
func test_a_non_emissive_material_is_unchanged() -> void:
	var w := make_world()
	w.hooks().debug_apply_sphere_paint(Vector3(20.0, 56.2, 20.0), 16.0, 1)
	settle(w)
	var d: Dictionary = w.hooks().debug_deferred_probe(
		Vector3(20.0, 75.0, 20.0), Vector3(0, -1, 0), 64, 64, 0)
	var c: Color = d["center"]
	assert_float(maxf(c.r, maxf(c.g, c.b))).is_less(1.0)
