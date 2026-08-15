extends GdUnitTestSuite

# Regression test for the task-11 magenta artifact: rays whose hit point converges to
# within ~1cm of a brick face used to round into the adjacent AIR MARGIN brick (activated
# by the generation pad but holding an EMPTY palette), resolving material 0 = error magenta.
# The material lookup is now anchored to the hit brick (the one whose SDF crossing was
# found), so these rays must return real terrain albedos.
#
# The five directions are the exact previously-magenta rays (camera (8,14,8), demo
# world 60x20x60): each previously produced a magenta core in the full-frame raymarch.
# The full-frame hit points were: (43.216,7.201,24.034), (39.995,7.203,24.473),
# (39.863,7.202,24.672), (15.016,0.805,12.001), (29.578,0.805,20.344).
#
# M2 keeps those world-space hit points relative to the camera: Errata 9 adds the +51.2
# surface offset to the generator field, so the terrain is the same shape translated
# +51.2 m in y; the camera here is likewise (8, 14+51.2, 8), and the same rays hit the
# same (x, z) columns at y = old_hit_y + 51.2.

const DEMO_ORIGIN := Vector3(8, 66.4, 8)
const MAGENTA_RAYS := [
	Vector3(0.89638, -0.17306, 0.40811),
	Vector3(0.87364, -0.18558, 0.44979),
	Vector3(0.87062, -0.18576, 0.45554),
	Vector3(0.45350, -0.85290, 0.25865),
	Vector3(0.76669, -0.46885, 0.43859),
]

# M2: the world is GPU-generated and streamed around a camera. The radius must cover the
# FARTHEST ray's hit point (the magenta regression rays land ~40 m out), which the sizing
# rule of thumb (see test_streaming.gd) puts at ~25k bricks in the worst case.
const ATLAS := Vector3i(48, 24, 32)   # 36864 slots (~380 MB on the test device)
const REGION_SLOTS := 64              # a 45 m ball intersects ~47 regions; leave headroom

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = ATLAS
	w.max_region_slots = REGION_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(4, 5, 4)
	w.residency_radius_m = 45.0
	add_child(w)
	w.ensure_initialized()
	# Settle at THIS suite's DEMO_ORIGIN, not the pixel suite's CAM: the regression rays
	# land ~40 m from DEMO_ORIGIN, and settling anywhere else would leave those hit
	# regions non-resident.
	var _quiet := 0
	for i in range(120):
		_quiet = _quiet + 1 if w.debug_stream_frame(DEMO_ORIGIN) == 0 else 0
		if _quiet >= 6:
			break
	return w

# Rejects error-magenta (g == 0) and a sky miss (sky-down r == 0.5498 after the rgba16f
# round-trip; every terrain albedo at any lambert term is r <= 0.50), while accepting any
# real grass/rock/dirt albedo: (c.r < 0.52 and c.g > 0.05).
func test_brick_face_hits_resolve_to_real_materials() -> void:
	var w := make_world()
	for dir in MAGENTA_RAYS:
		var c: Color = w.debug_raymarch_pixel(DEMO_ORIGIN, dir)
		assert_bool(c.r < 0.52 and c.g > 0.05).is_true() \
			.override_failure_message("ray %s returned %s (expected a real terrain material, not magenta/sky)" % [dir, c])
