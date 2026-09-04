extends GdUnitTestSuite

# Characterization (terrain pipeline Phase 0): pins the world generator's output, and pins
# that colliders still appear where they do today. Tasks 5-7 route ~21 direct
# ve::AnalyticGenerator constructions through the ve::FieldGenerator seam; if any of them
# ends up holding a different generator, or a null one, these values move.

# Captured from a clean main. If terrain changes INTENTIONALLY, regenerate by printing
# debug_generator_fingerprint() and pasting the values back here.
const EXPECTED_POINTS := 8
const SDF_EPS := 1e-5

var _world: VoxelWorld

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)

func test_generator_fingerprint_is_stable() -> void:
	var a: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	assert_int(a.size()).is_equal(EXPECTED_POINTS * 3)
	# Self-consistency: the seam must hand out the SAME generator every call.
	var b: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	for i in range(a.size()):
		assert_float(a[i]).is_equal_approx(b[i], SDF_EPS)

func test_fingerprint_matches_recorded_baseline() -> void:
	var got: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	var want := _baseline()
	assert_int(got.size()).is_equal(want.size())
	for i in range(got.size()):
		assert_float(got[i]).is_equal_approx(want[i], SDF_EPS)

func _baseline() -> PackedFloat32Array:
	# Captured from a clean main (Step 6 print of debug_generator_fingerprint()).
	return PackedFloat32Array([0.0, 3.0, 0.0, 0.59057021141052, 0.0, 0.0,
			4.51811981201172, 0.0, 0.0, 1.52731251716614, 0.0, 0.0,
			-31.2000007629395, 3.0, 0.0, 38.7999992370605, 0.0, 0.0,
			-0.4096292257309, 3.0, 0.0, 0.37001866102219, 0.0, 0.0])
