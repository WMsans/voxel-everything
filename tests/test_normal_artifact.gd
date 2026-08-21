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

# ---------------------------------------------------------------------------------------
# Task 7: the remaining surface classes shade from their stored/source normals too.
# Thresholds include RG8 oct error but exclude the old R8 SDF curls.
# ---------------------------------------------------------------------------------------

func quiet_stream(world: VoxelWorld, centre: Vector3) -> void:
    var quiet := 0
    for frame in range(500):
        quiet = quiet + 1 if world.debug_stream_frame(centre) == 0 else 0
        if quiet >= 6:
            break

func disable_effects(world: VoxelWorld) -> void:
    for e in ["ssgi", "ssr", "contact_shadows", "outlines", "sun_shadow_map",
            "glossy_sdf_rays", "raymarched_sun_shadow"]:
        world.set_effect_enabled(e, false)

func make_op_bytes(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
    var b := StreamPeerBuffer.new()
    b.big_endian = false
    b.put_u32(type)
    b.put_u32(material)
    b.put_float(pos.x)
    b.put_float(pos.y)
    b.put_float(pos.z)
    b.put_float(radius)
    b.put_u32(0)
    b.put_u32(0)
    return b.data_array

func hills(x: float, z: float) -> float:
    return 6.0 * sin(x * 0.11) * cos(z * 0.13) \
        + 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043) \
        + 1.0 * sin(x * 0.23 + z * 0.19)

# A sphere ADD shades from the exact procedural gradient of its winning CSG branch.
# Rays whose winning branch is at least 0.02 m from a tie must align with the CPU
# source-field normal to within oct/float noise (alignment > 0.995).
func test_a_sphere_add_shades_from_the_source_gradient(timeout := 90000) -> void:
    var world := make_world()
    disable_effects(world)
    var centre := Vector3(24.0, 58.0, 24.0)
    var radius := 3.0
    world.debug_apply_sphere_add(centre, radius, 4)
    quiet_stream(world, Vector3(24.0, 56.0, 24.0))

    var ops := make_op_bytes(1, 4, centre, radius) # OP_SPHERE_ADD
    var aligned := 0
    var worst := 1.0
    for iz in range(9):
        for ix in range(9):
            var off := Vector3((ix - 4) * 0.55, 8.0, (iz - 4) * 0.55)
            var probe: Dictionary = world.debug_raymarch_gbuffer(centre + off, Vector3(0, -1, 0))
            if not probe["hit"]:
                continue
            var pos: Vector3 = probe["position"]
            # Only hits that belong to the added sphere's winning branch.
            var sp: float = (pos - centre).length() - radius
            var terrain_sdf: float = pos.y - 51.2 - hills(pos.x, pos.z)
            if absf(sp - terrain_sdf) < 0.02 or sp > 0.05:
                continue
            var cpu: Dictionary = world.debug_eval_field_gradient(pos, ops, 1)
            if not bool(cpu["exact"]):
                continue
            var n: Vector3 = probe["normal"]
            var dot: float = n.dot(cpu["gradient"].normalized())
            worst = minf(worst, dot)
            aligned += 1
    assert_int(aligned).override_failure_message(
        "no ray landed on the added sphere's own branch").is_greater(10)
    assert_float(worst).override_failure_message(
        "worst sphere-add normal alignment %.5f (want > 0.995)" % worst).is_greater(0.995)

# A stored volume added into the field shades from its compact normals (batch probe).
# The authoritative GPU pool holds fixed 64^3 slots, so the fixture uses a full-size
# lattice at the standard 5 cm pitch.
func test_a_volume_add_shades_from_its_stored_normals(timeout := 90000) -> void:
    var world := make_world()
    disable_effects(world)
    const DIM := 64
    var sdf := PackedByteArray()
    sdf.resize(DIM * DIM * DIM)
    var mat := PackedByteArray()
    mat.resize(DIM * DIM * DIM)
    var encode := func(d: float) -> int:
        return int(clampf((d + 0.64) / 1.28, 0.0, 1.0) * 255.0 + 0.5)
    var c := float(DIM - 1) * 0.5
    for z in range(DIM):
        for y in range(DIM):
            for x in range(DIM):
                var d := Vector3(x - c, y - c, z - c).length() * 0.05 - 0.26
                var i := x + y * DIM + z * DIM * DIM
                sdf[i] = encode.call(d)
                mat[i] = 4 if d <= 0.0 else 0
    # A RENDER-REACHABLE stored volume: brick_gen and the shading gradient both read the
    # shared authoritative volume/normal buffers, so the fixture must use the committed
    # upload hook (CPU store + pin + queued GPU handoff into BOTH pools), then drain the
    # handoff before the op that names the slot is baked or shaded.
    world.debug_queue_committed_field_volume_upload(0, sdf, mat, DIM)
    world.debug_island_frame(1.0 / 60.0, Vector3(18.4, 56.2, 18.4)) # drain the GPU handoff
    var origin := Vector3(18.0, 62.0, 18.0)
    world.debug_apply_volume_add(0, origin, 0.05, DIM)
    quiet_stream(world, Vector3(18.4, 56.2, 18.4))

    var result: Dictionary = world.debug_raymarch_normal_probe(
        Vector3(18.4, 72.0, 18.4), Vector3(0, -1, 0), 160, 120)
    assert_bool(result.get("ran", false)).is_true()
    assert_int(result.get("hits", 0)).is_greater(4000)
    assert_float(result.get("rms_ndl", 1.0)).override_failure_message(
        "volume-add RMS N.L %.5f (want <= 0.003)" % result.get("rms_ndl", 1.0)
        ).is_less_equal(0.003)
    assert_float(result.get("cel_mismatch_fraction", 1.0)).is_less(0.001)
    assert_int(result.get("largest_mismatch_component", 999999)).is_less_equal(8)

# After consolidation the crater walls shade from the baked override normals (batch probe).
func test_a_consolidated_region_shades_from_override_normals(timeout := 120000) -> void:
    var world := make_world()
    disable_effects(world)
    world.debug_apply_sphere_subtract(Vector3(24.4, 51.4, 24.4), 2.0)
    world.debug_apply_sphere_subtract(Vector3(27.0, 51.4, 24.4), 1.5)
    assert_bool(world.debug_consolidate_region(Vector3i(0, 2, 0))).is_true()
    quiet_stream(world, Vector3(25.7, 56.2, 24.4))

    var result: Dictionary = world.debug_raymarch_normal_probe(
        Vector3(25.7, 70.0, 24.4), Vector3(0, -1, 0), 160, 120)
    assert_bool(result.get("ran", false)).is_true()
    assert_int(result.get("hits", 0)).is_greater(4000)
    assert_float(result.get("rms_ndl", 1.0)).override_failure_message(
        "consolidated-override RMS N.L %.5f (want <= 0.003)" % result.get("rms_ndl", 1.0)
        ).is_less_equal(0.003)
    assert_float(result.get("cel_mismatch_fraction", 1.0)).is_less(0.001)
    assert_int(result.get("largest_mismatch_component", 999999)).is_less_equal(8)

# A placed, rotated island shades from its OWN compact normals in the body frame.
func test_a_placed_rotated_island_shades_from_body_normals(timeout := 90000) -> void:
    var world := make_world()
    disable_effects(world)
    var d: Dictionary = world.debug_place_test_island_rotated(0,
        Vector3i(25, 58, 25), Vector3i(28, 59, 26), Vector3(0.0, 30.0, 0.0), PI * 0.5)
    assert_bool(d.get("ok", false)).is_true()
    var centre: Vector3 = d["world_center"]
    var result: Dictionary = world.debug_island_normal_probe(
        0, centre + Vector3(0.0, 6.0, 0.0), Vector3(0, -1, 0), 128, 96)
    assert_bool(result.get("ran", false)).is_true()
    assert_int(result.get("hits", 0)).is_greater(200)
    assert_float(result.get("rms_ndl", 1.0)).override_failure_message(
        "island RMS N.L %.5f (want <= 0.003)" % result.get("rms_ndl", 1.0)
        ).is_less_equal(0.003)
    assert_float(result.get("cel_mismatch_fraction", 1.0)).is_less(0.001)
    assert_int(result.get("largest_mismatch_component", 999999)).is_less_equal(8)
