extends GdUnitTestSuite
# The importability promise (spec §4): voxel_world.tscn works under ANY parent, not only at
# /root/Main, and two instances never share render state.

const WORLD_SCENE := "res://demo/scenes/voxel_world.tscn"

var _holders: Array = []

func after_test() -> void:
	for h in _holders:
		if is_instance_valid(h):
			h.free()
	_holders.clear()

func _place(parent_name: String) -> VoxelWorldScene:
	var holder := Node3D.new()
	holder.name = parent_name
	get_tree().root.add_child(holder)
	_holders.append(holder)
	var w: VoxelWorldScene = load(WORLD_SCENE).instantiate()
	w.use_local_device = true
	w.physics_enabled = false
	holder.add_child(w)
	return w

func _effects(w: Node) -> Array:
	return (w.get_node("WorldEnvironment") as WorldEnvironment).compositor.compositor_effects

func test_compositors_follow_the_instance_wherever_it_is_placed() -> void:
	var w := _place("SomeOtherGame")
	assert_str(str(w.get_path())).is_equal("/root/SomeOtherGame/VoxelWorld")
	var fx := _effects(w)
	assert_int(fx.size()).is_equal(2)
	for e in fx:
		assert_object(get_tree().root.get_node_or_null(e.world_path)).is_same(w)

func test_two_instances_never_share_a_compositor() -> void:
	var a := _place("A")
	var b := _place("B")
	var fa := _effects(a)
	var fb := _effects(b)
	assert_int(fa.size()).is_equal(fb.size())
	for i in range(fa.size()):
		assert_object(fa[i]).is_not_same(fb[i])
		assert_object(get_tree().root.get_node_or_null(fa[i].world_path)).is_same(a)
		assert_object(get_tree().root.get_node_or_null(fb[i].world_path)).is_same(b)

func test_sun_and_settings_resolve_inside_the_instance() -> void:
	var w := _place("Elsewhere")
	assert_object(w.get_node_or_null(w.sun_light_path)).is_same(w.get_node("Sun"))
	var vs: VoxelSettings = w.get_node("VoxelSettings")
	assert_object(vs.get_node_or_null(vs.world_path)).is_same(w)

func test_the_world_type_sets_the_pipeline_before_the_tree() -> void:
	var w: VoxelWorldScene = load(WORLD_SCENE).instantiate()
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/default.pipeline")
	w.world_type = load("res://demo/world_types/10_mesas.tres")
	assert_bool(w.is_inside_tree()).is_false()
	assert_str(w.terrain_pipeline_path).is_equal("res://assets/pipelines/mesas.pipeline")
	w.free()

func test_ground_at_is_null_before_the_world_initialises() -> void:
	var w: VoxelWorldScene = load(WORLD_SCENE).instantiate()
	assert_that(w.ground_at(8.0, 8.0)).is_null()
	w.free()

func test_ground_at_finds_the_default_surface_once_initialised() -> void:
	var w := _place("Ground")
	assert_bool(w.hooks().debug_init_atlas()).is_true()
	var hit = w.ground_at(8.0, 8.0)
	assert_that(hit).is_not_null()
	# SURFACE_Y 51.2 plus hills (|h| <= 10 m); relief is ~0 this close to the origin.
	assert_float((hit as Vector3).y).is_between(41.0, 62.0)
