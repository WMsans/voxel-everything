extends GdUnitTestSuite

# If a mip cell ever wrongly reports "no surface" the ray TUNNELS: these rays all hit, and
# the probe's diagnostics pin whether a failure came from the mips or the march logic.
const ATLAS := Vector3i(32, 16, 32)
const REGION_SLOTS := 16
const CAM := Vector3(20, 56.2, 20)

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(4, 5, 4)
	w.residency_radius_m = 20.0
	add_child(w)
	w.ensure_initialized()
	for i in range(60):
		if w.debug_stream_frame(CAM) == 0:
			break
	return w

func test_down_rays_hit_and_the_mips_at_the_hit_agree() -> void:
	var w := make_world()
	for ox in range(-4, 5):
		for oz in range(-4, 5):
			var d: Dictionary = w.debug_raymarch_probe(
				Vector3(20 + ox, 56.2, 20 + oz), Vector3(0, -1, 0))
			assert_bool(d["hit"]).override_failure_message(
				"tunnel at offset %d,%d" % [ox, oz]).is_true()
			assert_bool(d["brick_surface"]).is_true()
			assert_bool(d["cell8_surface"]).is_true()

func test_diagonal_ray_hits_through_many_empty_cells() -> void:
	var w := make_world()
	# Grazing descent: crosses dozens of empty 8^3 cells before reaching the surface.
	var d: Dictionary = w.debug_raymarch_probe(Vector3(12, 56.2, 20), Vector3(0.5, -0.6, 0))
	assert_bool(d["hit"]).is_true()
	assert_bool(d["brick_surface"]).is_true()
	assert_bool(d["cell8_surface"]).is_true()
	# And it must be the REAL surface: material colours, not sky or magenta.
	var c: Color = d["color"]
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()
