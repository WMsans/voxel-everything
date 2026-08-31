extends GdUnitTestSuite

var _worlds: Array = []

func after_test() -> void:
	for w in _worlds:
		if is_instance_valid(w):
			w.free()
	_worlds.clear()

# No streaming and no atlas init: material_table() is a constant table read, and a test
# that needed a GPU to read a constant would be paying 20 seconds for nothing.
func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.physics_enabled = false
	add_child(w)
	_worlds.append(w)
	return w

func test_the_table_reaches_gdscript() -> void:
	var t: Array = make_world().material_table()
	assert_int(t.size()).is_greater_equal(4)
	var first: Dictionary = t[0]
	for key in ["id", "name", "asset", "hardness", "glow", "glow_color", "albedo"]:
		assert_bool(first.has(key)).override_failure_message(
			"material_table() entry is missing '%s'" % key).is_true()

# Ids are what the edit ops carry, and layer i serves id i + 1. A picker that wrote the
# ARRAY INDEX into fill_material would paint the wrong material with no visible error.
func test_ids_are_one_based_and_ascending() -> void:
	var t: Array = make_world().material_table()
	for i in range(t.size()):
		assert_int(t[i]["id"]).is_equal(i + 1)

func test_every_material_is_at_least_baseline_hardness() -> void:
	for m in make_world().material_table():
		assert_float(m["hardness"]).override_failure_message(
			"%s has hardness below 1.0, which would enlarge nominal removal shapes" % m["name"]
			).is_greater_equal(1.0)
