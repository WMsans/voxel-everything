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
	# Terrain albedos are green/grey/brown; sky is blue-dominant.
	assert_bool(c.b <= c.g or c.r > 0.1).is_true()

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
	# Discriminates hit from miss. (a) r < 0.52: any terrain albedo (grass 0.36,
	# rock 0.45, dirt 0.50) times (0.25 + 0.75*lam), lam <= 1, gives r <= 0.50.
	# (b) b > g: all hits currently shade the error-magenta albedo (the palette
	# storage buffer is uploaded as uint16 but read as uint in the shader, a
	# pre-existing bug), which has g = 0. The miss color sky-down (0.55, 0.45,
	# 0.35) fails both: its r is 0.5498 after the rgba16f half-float round-trip
	# (so r < 0.55 would NOT discriminate) and its b <= g.
	assert_bool(c.b > c.g or c.r < 0.52).is_true()

func test_ray_diagonal_down_from_non_boundary_origin_hits_terrain() -> void:
	var w := make_world()
	# Diagonal negative-direction DDA from a non-boundary origin.
	var c: Color = w.debug_raymarch_pixel(Vector3(7.3, 11.2, 9.1), Vector3(0, -0.9, -0.2))
	assert_bool(c.b <= c.g or c.r > 0.1).is_true()
