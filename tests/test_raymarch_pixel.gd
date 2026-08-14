extends GdUnitTestSuite

func make_world() -> VoxelWorld:
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.world_size_bricks = Vector3i(20, 12, 20)
	add_child(w)
	w.ensure_initialized()
	return w

func test_ray_down_from_sky_hits_terrain() -> void:
	var w := make_world()
	# From (8, 12, 8) looking straight down: hills here are ~3m, must hit.
	var c: Color = w.debug_raymarch_pixel(Vector3(8, 12, 8), Vector3(0, -1, 0))
	# Reject both a sky miss and the error-magenta albedo while accepting real
	# terrain colors: (a) r < 0.52 — any terrain albedo (grass 0.36, rock 0.45,
	# dirt 0.50) times (0.25 + 0.75*lam), lam <= 1, gives r <= 0.50, and sky-down
	# r is 0.5498 after the rgba16f round-trip; (b) g > 0.2 — real grass/rock/dirt
	# have g >= ~0.22, while magenta (the palette uint16/uint mismatch) has g = 0.
	assert_bool((c.b > c.g or c.r < 0.52) and c.g > 0.2).is_true()

func test_ray_up_from_air_misses_to_sky() -> void:
	var w := make_world()
	var c: Color = w.debug_raymarch_pixel(Vector3(8, 8, 8), Vector3(0, 1, 0))
	# Sky gradient looking up is blue-dominant.
	assert_bool(c.b > c.r).is_true()

func test_ray_down_from_non_boundary_origin_hits_terrain() -> void:
	var w := make_world()
	# Origin NOT on a brick boundary (8.25, 12.3, 7.9 are not multiples of 0.8):
	# the negative-direction DDA must still reach terrain (~3.1m here).
	var c: Color = w.debug_raymarch_pixel(Vector3(8.25, 12.3, 7.9), Vector3(0, -1, 0))
	# Discriminates hit from miss, AND true albedo from error-magenta. (a) r < 0.52:
	# any terrain albedo (grass 0.36, rock 0.45, dirt 0.50) times (0.25 + 0.75*lam),
	# lam <= 1, gives r <= 0.50; the miss color sky-down has r = 0.5498 after the
	# rgba16f round-trip. (b) g > 0.2: real grass/rock/dirt have g >= ~0.22, while
	# magenta (the palette uint16/uint mismatch albedo, g = 0) fails — (b > g or
	# r < 0.52) alone is vacuous for magenta since 0.85 > 0.
	assert_bool((c.b > c.g or c.r < 0.52) and c.g > 0.2).is_true()

func test_ray_diagonal_down_from_non_boundary_origin_hits_terrain() -> void:
	var w := make_world()
	# Diagonal negative-direction DDA from a non-boundary origin.
	var c: Color = w.debug_raymarch_pixel(Vector3(7.3, 11.2, 9.1), Vector3(0, -0.9, -0.2))
	# Measured hit color (grass albedo, rgba16f round-trip): (0.3066, 0.4685, 0.1874).
	# Sky miss for the SAME direction (measured from a high origin): (0.5464, 0.45, 0.3557)
	# — r >= 0.52 makes (b > g or r < 0.52) false, so a sky miss is rejected.
	# Error-magenta (r≈b>0.7, g=0): g = 0 fails the g > 0.05 gate.
	assert_bool((c.b > c.g or c.r < 0.52) and c.g > 0.05).is_true()

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
			var c: Color = w.debug_raymarch_pixel(Vector3(x, 9.0, z), Vector3(0, -1, 0))
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
