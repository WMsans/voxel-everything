extends GdUnitTestSuite

var _worlds: Array[VoxelWorld] = []

func after_test() -> void:
    for world in _worlds:
        if is_instance_valid(world):
            world.free()
    _worlds.clear()

func make_world() -> VoxelWorld:
    var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
    world.use_local_device = true
    world.physics_enabled = false
    world.atlas_bricks = Vector3i(32, 16, 32)
    world.max_region_slots = 64
    world.world_origin_bricks = Vector3i(0, -64, 0)
    world.world_size_regions = Vector3i(8, 5, 8)
    add_child(world)
    _worlds.append(world)
    assert_bool(world.debug_init_atlas()).is_true()
    var quiet := 0
    for frame in range(500):
        quiet = quiet + 1 if world.debug_stream_frame(Vector3(20, 56, 20)) == 0 else 0
        if quiet >= 6:
            break
    return world

func test_a_broad_terrain_patch_has_no_r8_normal_curls(timeout := 90000) -> void:
    var world := make_world()
    world.set_effect_enabled("ssgi", false)
    world.set_effect_enabled("ssr", false)
    world.set_effect_enabled("contact_shadows", false)
    world.set_effect_enabled("outlines", false)
    world.set_effect_enabled("sun_shadow_map", false)
    world.set_effect_enabled("glossy_sdf_rays", false)
    world.set_effect_enabled("raymarched_sun_shadow", false)
    var result: Dictionary = world.debug_raymarch_normal_probe(
        Vector3(20.0, 72.0, 29.0), Vector3(0.1, -0.85, -0.5).normalized(), 256, 192)
    assert_bool(result.get("ran", false)).is_true()
    assert_int(result.get("hits", 0)).is_greater(12000)
    assert_float(result.get("rms_ndl", 1.0)).is_less_equal(0.001)
    assert_float(result.get("cel_mismatch_fraction", 1.0)).is_less(0.001)
    assert_int(result.get("largest_mismatch_component", 999999)).is_less_equal(8)
