extends GdUnitTestSuite

# M2: the world is GPU-generated and streamed around a camera. The radius must cover the
# FARTHEST ray's hit point (the magenta regression rays land ~40 m out), which the sizing
# rule of thumb (see test_streaming.gd) puts at ~25k bricks in the worst case.
const ATLAS := Vector3i(48, 24, 32)   # 36864 slots (~380 MB on the test device)
const REGION_SLOTS := 64              # a 45 m ball intersects ~47 regions; leave headroom
# 56.2 = 51.2 + 5: just above the local surface (surface sits at 51.2 + hills, hills +-10).
const CAM := Vector3(20, 56.2, 20)

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
	# Settle AT CAM: this suite's rays (x,z in [5, 13], hits y ~ 52-58 m) all land within
	# 45 m of it, so settling anywhere else could leave their hit regions non-resident.
	var _quiet := 0
	for i in range(120):
		_quiet = _quiet + 1 if w.debug_stream_frame(CAM) == 0 else 0
		if _quiet >= 6:
			break
	return w

func is_magenta(c: Color) -> bool:
	# material_surface()'s out-of-range fallback is (1, 0, 1); after shading, error magenta
	# keeps g == 0 while every real material has a non-zero green channel.
	return c.g < 0.05 and c.r > 0.2

# Re-baselined for M5: the near field is textured now, so exact albedo ranges no longer
# apply. These tests assert the hit/miss structure (debug_raymarch_probe's hit flag) and
# the magenta fallback (a hit must not be error magenta) instead of exact colours.
func test_ray_down_from_sky_hits_terrain() -> void:
	var w := make_world()
	# From (8, 63.2, 8) looking straight down: hills here are ~3m, must hit.
	var d: Dictionary = w.debug_raymarch_probe(Vector3(8, 63.2, 8), Vector3(0, -1, 0))
	assert_bool(d["hit"]).override_failure_message("ray down from sky missed").is_true()
	assert_bool(not is_magenta(d["color"])).override_failure_message(
		"hit shaded error magenta").is_true()

func test_ray_up_from_air_misses_to_sky() -> void:
	var w := make_world()
	var d: Dictionary = w.debug_raymarch_probe(Vector3(8, 59.2, 8), Vector3(0, 1, 0))
	assert_bool(not d["hit"]).override_failure_message("ray up from air hit terrain").is_true()

func test_ray_down_from_non_boundary_origin_hits_terrain() -> void:
	var w := make_world()
	# Origin NOT on a brick boundary (8.25, 63.5, 7.9 are not multiples of 0.8):
	# the negative-direction DDA must still reach terrain (~3.1m here).
	var d: Dictionary = w.debug_raymarch_probe(
		Vector3(8.25, 63.5, 7.9), Vector3(0, -1, 0))
	assert_bool(d["hit"]).override_failure_message(
		"non-boundary ray down from sky missed").is_true()
	assert_bool(not is_magenta(d["color"])).override_failure_message(
		"non-boundary hit shaded error magenta").is_true()

func test_ray_diagonal_down_from_non_boundary_origin_hits_terrain() -> void:
	var w := make_world()
	# Diagonal negative-direction DDA from a non-boundary origin.
	var d: Dictionary = w.debug_raymarch_probe(
		Vector3(7.3, 62.4, 9.1), Vector3(0, -0.9, -0.2))
	assert_bool(d["hit"]).override_failure_message(
		"diagonal ray down from sky missed").is_true()
	assert_bool(not is_magenta(d["color"])).override_failure_message(
		"diagonal hit shaded error magenta").is_true()

const BRICK_SIZE := 0.8
const VOXEL_SIZE := 0.05

func hills(x: float, z: float) -> float: # test oracle, mirrors AnalyticGenerator
	return 6.0 * sin(x * 0.11) * cos(z * 0.13) \
		+ 3.0 * sin(x * 0.031 + 1.7) * sin(z * 0.043) \
		+ 1.0 * sin(x * 0.23 + z * 0.19)

func luminance(c: Color) -> float:
	return 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b

# Regression: the SDF brick apron. A brick stores a 17^3 lattice so trilinear
# reconstruction covers its whole [0,16) extent. Without the apron the last voxel slab
# clamps to a constant, the gradient along that axis collapses, calc_normal() returns a
# wrong normal, and every brick face draws a dark seam — the visible "stripes" on the
# terrain. Measured on the reference GPU: slab-15 minimum luminance was 0.121 without the
# apron (all other slabs >= 0.30) and 0.304 with it.
func test_brick_face_slab_is_not_darker_than_the_rest() -> void:
	var w := make_world()
	var lum_face := []   # hits landing in the last voxel slab of a brick
	var lum_other := []  # hits landing anywhere else
	for i in range(60):
		for j in range(60):
			var x := 5.0 + i * 0.13
			var z := 5.0 + j * 0.11
			var h := hills(x, z)
			if h < 1.0 or h > 7.0:
				continue
			var c: Color = w.debug_raymarch_pixel(Vector3(x, 60.2, z), Vector3(0, -1, 0))
			if c.b > c.g:
				continue # sky miss
			var slab := int(floor(fposmod(h, BRICK_SIZE) / VOXEL_SIZE))
			if slab == 15:
				lum_face.append(luminance(c))
			else:
				lum_other.append(luminance(c))
	assert_int(lum_face.size()).is_greater(50)
	assert_int(lum_other.size()).is_greater(500)

	var face_min: float = lum_face.min()
	var other_min: float = lum_other.min()
	var face_mean: float = 0.0
	for v in lum_face:
		face_mean += v
	face_mean /= lum_face.size()
	var other_mean: float = 0.0
	for v in lum_other:
		other_mean += v
	other_mean /= lum_other.size()

	# No pixel on a brick face may be dramatically darker than the darkest ordinary pixel,
	# and the face slab's average shading must track the rest of the surface.
	assert_float(face_min).is_greater(other_min * 0.75)
	assert_float(absf(face_mean - other_mean)).is_less(0.02)
