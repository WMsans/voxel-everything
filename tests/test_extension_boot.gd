extends GdUnitTestSuite

func test_voxel_world_class_registered() -> void:
	assert_bool(ClassDB.class_exists("VoxelWorld")).is_true()
	assert_bool(ClassDB.can_instantiate("VoxelWorld")).is_true()

func test_voxel_world_instantiates() -> void:
	var node: Node3D = ClassDB.instantiate("VoxelWorld")
	assert_object(node).is_not_null()
	node.free()
