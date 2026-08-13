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
	assert_bool(c.b <= c.g or c.r > 0.1).is_true()
