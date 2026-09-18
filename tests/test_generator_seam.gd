extends GdUnitTestSuite

# Characterization (terrain pipeline Phase 0): pins the world generator's output, and pins
# that colliders still appear where they do today. Tasks 5-7 route ~21 direct
# ve::AnalyticGenerator constructions through the ve::FieldGenerator seam; if any of them
# ends up holding a different generator, or a null one, these values move.

# Captured after initializing the shipped default.pipeline. If terrain changes
# INTENTIONALLY, regenerate by printing debug_generator_fingerprint() and pasting the values
# back here.
const EXPECTED_POINTS := 8
const SDF_EPS := 1e-5

var _world: VoxelWorld

func before_test() -> void:
	_world = ClassDB.instantiate("VoxelWorld")
	add_child(_world)

func _init_world() -> void:
	assert_bool(_world.hooks().debug_init_atlas()).is_true()

func test_generator_fingerprint_is_stable() -> void:
	_init_world()
	var a: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	assert_int(a.size()).is_equal(EXPECTED_POINTS * 3)
	# Self-consistency: the seam must hand out the SAME generator every call.
	var b: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	for i in range(a.size()):
		assert_float(a[i]).is_equal_approx(b[i], SDF_EPS)

func test_failed_pipeline_init_keeps_field_hooks_safe() -> void:
	_world.terrain_pipeline_path = "res://assets/pipelines/missing.pipeline"
	assert_bool(_world.hooks().debug_init_atlas()).is_false()
	var sample := _world.hooks().debug_eval_field(Vector3.ZERO, PackedByteArray(), 0)
	assert_float(sample.x).is_equal(0.0)
	assert_float(sample.y).is_equal(0.0)
	assert_bool(_world.hooks().debug_brick_has_surface(Vector3i.ZERO, PackedByteArray(), 0)).is_false()

func test_fingerprint_matches_recorded_baseline() -> void:
	_init_world()
	var got: PackedFloat32Array = _world.hooks().debug_generator_fingerprint()
	var want := _baseline()
	assert_int(got.size()).is_equal(want.size())
	for i in range(got.size()):
		assert_float(got[i]).is_equal_approx(want[i], SDF_EPS)

func _baseline() -> PackedFloat32Array:
	# Captured from the initialized default.pipeline world after relief became part of the
	# shipped field (the old pre-pipeline values are no longer the active terrain).
	return PackedFloat32Array([0.0, 3.0, 0.0, -1.25439250469208, 2.0, 0.0,
			4.51811981201172, 0.0, 0.0, 6.02639389038086, 0.0, 0.0,
			-31.2000007629395, 3.0, 0.0, 38.7999992370605, 0.0, 0.0,
			-104.217163085938, 2.0, 0.0, 104.177551269531, 0.0, 0.0])
