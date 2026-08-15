extends GdUnitTestSuite

# The destruction pipeline end to end: tool -> edit log -> op upload -> re-mark ->
# indirect regeneration -> visible in the raymarcher, with the analytic raycast as oracle.
const ATLAS := Vector3i(32, 16, 32)
const REGION_SLOTS := 16
const CAM := Vector3(40, 56.2, 40)

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

func make_tool(w: VoxelWorld) -> VoxelEditTool:
	var t: VoxelEditTool = ClassDB.instantiate("VoxelEditTool")
	w.add_child(t)
	return t

func make_op(type: int, material: int, pos: Vector3, radius: float) -> PackedByteArray:
	var b := StreamPeerBuffer.new()
	b.big_endian = false
	b.put_u32(type); b.put_u32(material)
	b.put_float(pos.x); b.put_float(pos.y); b.put_float(pos.z)
	b.put_float(radius)
	b.put_u32(0); b.put_u32(0)
	return b.data_array

func test_sphere_subtract_carves_a_visible_hole() -> void:
	var w := make_world()
	var tool := make_tool(w)
	var hit: Dictionary = w.debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]

	var before: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # solid terrain

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(hp.x, hp.y + 0.5, hp.z), 2.5)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(CAM)

	# The old surface point is now air: the ray lands in the crater, over a metre deeper,
	# and what it hits is still real terrain (not sky, not magenta).
	var after: Dictionary = w.debug_raycast(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_float(after["pos"].y).is_less(hp.y - 1.0)
	var c: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()

func test_sphere_add_places_material_4_in_open_sky() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# The surface stays below ~61.5 m everywhere, so 66-70 m is open air. Look DOWN from
	# above the blob: its top is sunlit (a ray from below would see the ambient-lit
	# underside at 0.25x albedo — too dim for a useful colour assertion).
	var eye := Vector3(40, 75.0, 40)
	var before: Color = w.debug_raymarch_pixel(eye, Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # distant terrain below

	var r: Dictionary = tool.apply_sphere_add(Vector3(40, 68.2, 40), 1.5, 4)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(CAM)

	# Material 4's albedo (0.62, 0.60, 0.66) is the only one with b > r.
	var c: Color = w.debug_raymarch_pixel(eye, Vector3(0, -1, 0))
	assert_bool(c.b > c.r and c.r > 0.35).is_true()

func test_paint_recolours_grass_to_rock_without_moving_the_surface() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# Find a GRASS spot deterministically: grass is the h in (1, 4) band.
	var hp := Vector3.ZERO
	var found := false
	for x in range(30, 50):
		for z in range(30, 50):
			var h: Dictionary = w.debug_raycast(Vector3(x, 80, z), Vector3(0, -1, 0))
			if h["hit"] and h["pos"].y - 51.2 > 1.0 and h["pos"].y - 51.2 < 4.0:
				hp = h["pos"]
				found = true
				break
		if found:
			break
	assert_bool(found).is_true()

	var before: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.g > before.r).is_true() # grass is green-dominant
	var r: Dictionary = tool.apply_sphere_paint(hp, 1.5, 2) # rock
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.debug_stream_frame(CAM)

	var after: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_bool(after.r > after.g).is_true() # rock (0.45, 0.42, 0.40) is red-leaning
	# The surface must not have moved: same ray, same hit depth to a centimetre.
	var depth: Dictionary = w.debug_raycast(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_float(depth["pos"].y).is_equal_approx(hp.y, 0.01)

func test_an_op_on_a_region_border_updates_both_sides() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# x = 25.6 m is the boundary between regions 0 and 1 on x. Settle next to the border:
	# the default CAM is >20 m from region 0's side of it, so it would not be resident.
	for i in range(60):
		if w.debug_stream_frame(Vector3(20, 53, 13)) == 0:
			break
	var hit: Dictionary = w.debug_raycast(Vector3(25.6, 80, 12.8), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var r: Dictionary = tool.apply_sphere_subtract(hp, 3.0)
	assert_array(r["rejected"]).is_empty()
	assert_int(r["touched"].size()).is_greater_equal(2) # both regions got the op
	for i in range(10):
		w.debug_stream_frame(Vector3(20, 53, 13))

	# Rebuild the op bytes the tool emitted and diff GPU bricks against the CPU reference
	# on BOTH sides of the border: each region's own op list drove its regeneration.
	var ops := make_op(0, 0, hp, 3.0)
	var checked := 0
	for bx in [31, 32]: # last brick of region 0, first of region 1
		for by in range(56, 72):
			var brick := Vector3i(bx, by, 16)
			var region := Vector3i(floori(bx / 32.0), floori(by / 32.0), 0)
			var rslot := w.debug_region_map_entry(region)
			if rslot < 0:
				continue
			var d: Dictionary = w.debug_brick_diff(brick, rslot, ops, 1)
			if int(d["slot"]) < 0:
				continue
			checked += 1
			assert_int(d["sdf_max_diff"]).is_less_equal(1)
			assert_bool(d["palette_match"]).is_true()
	assert_int(checked).override_failure_message(
		"no resident bricks found along the border").is_greater(0)

func test_a_full_region_rejects_the_257th_op_without_crashing() -> void:
	var w := make_world()
	var tool := make_tool(w)
	for i in range(256):
		tool.apply_sphere_subtract(Vector3(5.0, 0.0, 5.0), 0.1)
	var r: Dictionary = tool.apply_sphere_subtract(Vector3(5.0, 0.0, 5.0), 0.1)
	assert_array(r["rejected"]).is_not_empty()
	# Fail-soft: the world keeps streaming happily afterwards.
	assert_int(w.debug_stream_frame(CAM)).is_greater_equal(0)

func test_hostile_edit_inputs_are_rejected_before_touching_the_log() -> void:
	# Final-review Finding 2a: NaN/Inf positions or radius, a non-positive radius, or an
	# absurd radius must be refused BEFORE the edit log — the last one used to freeze
	# append for minutes (Finding 2b's loop), and NaN corrupts op_region_range (UB).
	var w := make_world()
	var tool := make_tool(w)
	var nan_r: Dictionary = tool.apply_sphere_subtract(Vector3(40, 60, 40), NAN)
	assert_array(nan_r["touched"]).is_empty()
	assert_array(nan_r["rejected"]).is_empty()
	var inf_r: Dictionary = tool.apply_sphere_add(Vector3(40, 60, 40), INF, 2)
	assert_array(inf_r["touched"]).is_empty()
	var zero_r: Dictionary = tool.apply_sphere_subtract(Vector3(40, 60, 40), 0.0)
	assert_array(zero_r["touched"]).is_empty()
	var neg_r: Dictionary = tool.apply_sphere_subtract(Vector3(40, 60, 40), -5.0)
	assert_array(neg_r["touched"]).is_empty()
	var huge_r: Dictionary = tool.apply_sphere_subtract(Vector3(40, 60, 40), 1.0e5)
	assert_array(huge_r["touched"]).is_empty()
	assert_array(huge_r["rejected"]).is_empty()
	var nan_p: Dictionary = tool.apply_sphere_subtract(Vector3(NAN, 60, 40), 2.0)
	assert_array(nan_p["touched"]).is_empty()
	# A material outside the 16-bit id range is clamped, not rejected.
	var add: Dictionary = tool.apply_sphere_add(Vector3(40, 68.2, 40), 1.5, 70000)
	assert_array(add["rejected"]).is_empty()
	# The world still streams normally after the rejected inputs.
	assert_int(w.debug_stream_frame(CAM)).is_greater_equal(0)

func test_edit_headroom_evicts_a_far_region_when_the_atlas_is_nearly_full() -> void:
	# Errata 11's eviction arm (spec §8 "evicts or drops"): when an SDF edit must
	# allocate slots and the free-slot count is below the 128-slot headroom, the streamer
	# evicts the furthest untouched resident region so the edit's marks succeed. The
	# config is tuned so the resident set leaves free = 117 slots at this camera
	# (atlas 26x16x26 = 10816 slots, 10 region slots): below the headroom, but the demand
	# still fits the atlas, so the settle never trips the free-list-empty fail-soft.
	const EVICT_ATLAS := Vector3i(26, 16, 26)
	const EVICT_SLOTS := 10
	var w: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	w.use_local_device = true
	w.atlas_bricks = EVICT_ATLAS
	w.max_region_slots = EVICT_SLOTS
	w.world_origin_bricks = Vector3i(0, -64, 0)
	w.world_size_regions = Vector3i(4, 5, 4)
	w.residency_radius_m = 20.0
	add_child(w)
	w.ensure_initialized()
	for i in range(60):
		if w.debug_stream_frame(CAM) == 0:
			break

	# Arm precondition: the resident set nearly fills the atlas (free < 128).
	var stats: Dictionary = w.debug_atlas_stats()
	assert_int(int(stats["free_slots"])).override_failure_message(
		"eviction-arm precondition unmet: free_slots not below the 128 headroom"
		).is_less(128)

	var tool := make_tool(w)
	var hit: Dictionary = w.debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var before: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # solid terrain

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(hp.x, hp.y + 0.5, hp.z), 2.5)
	assert_array(r["rejected"]).is_empty()

	# One frame: the streamer drains the edit, sees free < 128, and evicts the furthest
	# untouched resident region (the evicted region re-streams next frame).
	w.debug_stream_frame(CAM)
	var s: Dictionary = w.debug_stream_stats()
	assert_int(int(s["resident_regions"])).override_failure_message(
		"eviction arm did not run: no region was evicted for edit headroom"
		).is_equal(EVICT_SLOTS - 1)

	# Let the evicted region re-stream, then check the user-visible contract: the crater
	# is real (deeper hit, still terrain below) and no overflow bit was ever set.
	for i in range(10):
		w.debug_stream_frame(CAM)
	var after: Dictionary = w.debug_raycast(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_float(after["pos"].y).is_less(hp.y - 1.0)
	var c: Color = w.debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()
	assert_int(w.debug_stream_stats()["overflow_ever"]).is_equal(0)
