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
	settle(w, CAM)
	return w

# Settled means several consecutive quiet frames: the streamer paces stream-in against an
# atlas free count it reads back a frame or more behind the GPU, so a single frame with
# nothing to do is that pause, not the end of the work (six > its in-flight window).
func settle(w: VoxelWorld, cam: Vector3, frames := 120) -> void:
	var quiet := 0
	for i in range(frames):
		quiet = quiet + 1 if w.hooks().debug_stream_frame(cam) == 0 else 0
		if quiet >= 6:
			return


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
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]

	var before: Color = w.hooks().debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # solid terrain

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(hp.x, hp.y + 0.5, hp.z), 2.5)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.hooks().debug_stream_frame(CAM)

	# The old surface point is now air: the ray lands in the crater, over a metre deeper,
	# and what it hits is still real terrain (not sky, not magenta).
	var after: Dictionary = w.hooks().debug_raycast(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_float(after["pos"].y).is_less(hp.y - 1.0)
	var c: Color = w.hooks().debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()

func test_sphere_add_places_material_4_in_open_sky() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# The surface stays below ~61.5 m everywhere, so 66-70 m is open air. Look DOWN from
	# above the blob: its top is sunlit (a ray from below would see the ambient-lit
	# underside at 0.25x albedo — too dim for a useful colour assertion).
	var eye := Vector3(40, 75.0, 40)
	var before: Dictionary = w.hooks().debug_raymarch_probe(eye, Vector3(0, -1, 0))
	assert_bool(before["hit"]).is_true() # distant terrain below
	var before_color: Color = before["color"]
	assert_bool(before_color.r < 0.52 and before_color.g > 0.05).is_true()

	var r: Dictionary = tool.apply_sphere_add(Vector3(40, 68.2, 40), 1.5, 4)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.hooks().debug_stream_frame(CAM)

	# Material 4 is now the textured breakstone, not the old flat fill (0.62, 0.60, 0.66).
	# Re-baselined for M5: assert the add is visible (the ray stops on the blob high above
	# the terrain), is not error magenta, and changes the shaded pixel from the terrain below.
	var after: Dictionary = w.hooks().debug_raymarch_probe(eye, Vector3(0, -1, 0))
	assert_bool(after["hit"]).override_failure_message(
		"the ray no longer hit after adding material 4").is_true()
	assert_float(after["pos"].y).override_failure_message(
		"the sphere add did not place a visible blob in open sky").is_greater(before["pos"].y + 10.0)
	var c: Color = after["color"]
	assert_bool(c.g > 0.05).override_failure_message(
		"the material 4 blob shaded error magenta").is_true()
	# The claim this test exists to make -- "sphere_add places MATERIAL 4" -- read straight
	# off the G-buffer instead of inferred from a colour delta. Breakstone and the rock the
	# probe ray used to land on shade within ~0.02 of each other at this spot, so the delta
	# below sat about 18% above its own threshold and turned on a single 8-bit LSB of the
	# TERRAIN sample: any change that moves a hit by a fraction of a millimetre (the
	# marcher's filtered SDF read does, by 0.16 mm) flipped it, while the blob's own shaded
	# colour stayed bit-identical. Assert the identity exactly and keep the colour delta as
	# the secondary "it is not shading as the terrain" signal, at a threshold that measures
	# shading rather than rounding.
	var gb: Dictionary = w.hooks().debug_raymarch_gbuffer(eye, Vector3(0, -1, 0))
	assert_int(int(gb["material"])).override_failure_message(
		"the blob did not report material 4 in the G-buffer").is_equal(4)
	var diff := absf(c.r - before_color.r) + absf(c.g - before_color.g) + absf(c.b - before_color.b)
	assert_float(diff).override_failure_message(
		"the material 4 blob is not visibly different from the terrain below").is_greater(0.015)

func test_paint_recolours_grass_to_rock_without_moving_the_surface() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# Find a GRASS spot deterministically: grass is the h in (1, 4) band.
	var hp := Vector3.ZERO
	var found := false
	for x in range(30, 50):
		for z in range(30, 50):
			var h: Dictionary = w.hooks().debug_raycast(Vector3(x, 80, z), Vector3(0, -1, 0))
			if h["hit"] and h["pos"].y - 51.2 > 1.0 and h["pos"].y - 51.2 < 4.0:
				hp = h["pos"]
				found = true
				break
		if found:
			break
	assert_bool(found).is_true()

	# Re-baselined for M5: textured grass/rock no longer have stable flat-albedo colour
	# dominance, so assert the paint changes the shaded pixel and keeps the surface in place.
	var eye := Vector3(hp.x, hp.y + 1.0, hp.z)
	var before: Dictionary = w.hooks().debug_raymarch_probe(eye, Vector3(0, -1, 0))
	assert_bool(before["hit"]).override_failure_message(
		"the pre-paint ray missed the grass").is_true()
	var before_color: Color = before["color"]
	assert_bool(before_color.g > 0.05).override_failure_message(
		"the pre-paint grass shaded error magenta").is_true()
	var r: Dictionary = tool.apply_sphere_paint(hp, 1.5, 2) # rock
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.hooks().debug_stream_frame(CAM)

	var after: Dictionary = w.hooks().debug_raymarch_probe(eye, Vector3(0, -1, 0))
	assert_bool(after["hit"]).override_failure_message(
		"the post-paint ray missed the surface").is_true()
	var after_color: Color = after["color"]
	assert_bool(after_color.g > 0.05).override_failure_message(
		"the painted rock shaded error magenta").is_true()
	var diff := absf(after_color.r - before_color.r) + absf(after_color.g - before_color.g) + absf(after_color.b - before_color.b)
	assert_float(diff).override_failure_message(
		"painting material 2 did not visibly change the pixel colour").is_greater(0.02)
	# The surface must not have moved: same ray, same hit depth to a centimetre.
	var depth: Dictionary = w.hooks().debug_raycast(Vector3(hp.x, hp.y + 1.0, hp.z), Vector3(0, -1, 0))
	assert_float(depth["pos"].y).is_equal_approx(hp.y, 0.01)

func test_an_edit_leaves_no_brick_the_cpu_calls_active_without_a_slot() -> void:
	# The streamer re-marks exactly ve::op_brick_range after an edit, so that range has to
	# cover every brick the op flips. It is wider than the sphere for two reasons that are
	# easy to miss: the brick apron, and the activation pad — a CSG difference is a max, so
	# it raises the field OUTSIDE the sphere too, and a brick a few centimetres beyond the
	# crater goes from solidly interior to reporting a surface. Scan well past the op and
	# require the GPU's tables to agree with the CPU probe brick for brick.
	var w := make_world()
	var tool := make_tool(w)
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var r: Dictionary = tool.apply_sphere_subtract(hp, 2.5)
	assert_array(r["rejected"]).is_empty()
	for i in range(10):
		w.hooks().debug_stream_frame(CAM)

	var ops := make_op(0, 0, hp, 2.5)
	var touched := {}
	for t in r["touched"]:
		touched[Vector3i(t)] = true
	# The op's own range spans ~7 bricks; scan 12 either way so the flips beyond it count.
	var centre := Vector3i(floori(hp.x / 0.8), floori(hp.y / 0.8), floori(hp.z / 0.8))
	var active := 0
	var missing := 0
	for bx in range(centre.x - 12, centre.x + 13):
		for by in range(centre.y - 12, centre.y + 13):
			for bz in range(centre.z - 12, centre.z + 13):
				var brick := Vector3i(bx, by, bz)
				var region := Vector3i(floori(bx / 32.0), floori(by / 32.0), floori(bz / 32.0))
				var rslot: int = w.hooks().debug_region_map_entry(region)
				if rslot < 0:
					continue # not resident: its bricks arrive on stream-in
				# A brick is evaluated against ITS OWN region's op list, on both sides.
				var n := 1 if touched.has(region) else 0
				if not w.hooks().debug_brick_has_surface(brick, ops, n):
					continue
				active += 1
				if w.hooks().debug_region_table_slot(rslot, brick) < 0:
					missing += 1
	assert_int(active).override_failure_message(
		"the scan found no active bricks; check the aim").is_greater(0)
	assert_int(missing).override_failure_message(
		"%d of %d bricks the CPU calls active hold no atlas slot: the edit's re-mark range "
		% [missing, active] + "does not cover every brick its own probe flips").is_equal(0)

func test_an_op_on_a_region_border_updates_both_sides() -> void:
	var w := make_world()
	var tool := make_tool(w)
	# x = 25.6 m is the boundary between regions 0 and 1 on x. Settle next to the border:
	# the default CAM is >20 m from region 0's side of it, so it would not be resident.
	settle(w, Vector3(20, 53, 13))
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(25.6, 80, 12.8), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var r: Dictionary = tool.apply_sphere_subtract(hp, 3.0)
	assert_array(r["rejected"]).is_empty()
	assert_int(r["touched"].size()).is_greater_equal(2) # both regions got the op
	for i in range(10):
		w.hooks().debug_stream_frame(Vector3(20, 53, 13))

	# Rebuild the op bytes the tool emitted and diff GPU bricks against the CPU reference
	# on BOTH sides of the border: each region's own op list drove its regeneration.
	var ops := make_op(0, 0, hp, 3.0)
	var checked := 0
	for bx in [31, 32]: # last brick of region 0, first of region 1
		for by in range(56, 72):
			var brick := Vector3i(bx, by, 16)
			var region := Vector3i(floori(bx / 32.0), floori(by / 32.0), 0)
			var rslot := w.hooks().debug_region_map_entry(region)
			if rslot < 0:
				continue
			var d: Dictionary = w.hooks().debug_brick_diff(brick, rslot, ops, 1)
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
	assert_int(w.hooks().debug_stream_frame(CAM)).is_greater_equal(0)

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
	assert_int(w.hooks().debug_stream_frame(CAM)).is_greater_equal(0)

func test_an_edit_still_allocates_when_the_resident_set_fills_the_atlas() -> void:
	# Errata 11's eviction arm (spec §8 "evicts or drops") guarded the case where an SDF
	# edit must allocate slots and the atlas has none to give: the streamer evicts the
	# furthest untouched resident region so the edit's marks succeed.
	#
	# The streamer no longer lets streaming get that far — it holds a reserve back and caps
	# the resident set at what the atlas can hold — so the arm's old precondition (free < 128
	# after a plain settle) can no longer be produced, and asserting on it would only be
	# asserting that the starvation bug is still there. What is worth pinning is the
	# user-visible contract the arm existed for, which this config still exercises: with the
	# resident set filling a deliberately tight atlas, an edit's crater must be real and
	# nothing may be dropped.
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
	settle(w, CAM)

	# Precondition: the resident set really does fill this atlas — most of it is spoken for,
	# and what is left is the reserve the streamer keeps for edits.
	var stats: Dictionary = w.hooks().debug_atlas_stats()
	var used: int = int(stats["slot_count"]) - int(stats["free_slots"])
	assert_int(used).override_failure_message(
		"precondition unmet: the resident set does not fill the atlas (used %d of %d)"
		% [used, int(stats["slot_count"])]).is_greater(int(stats["slot_count"]) / 2)
	assert_int(int(stats["free_slots"])).override_failure_message(
		"streaming spent the whole atlas; an edit would have nothing to allocate from"
		).is_greater(0)

	var tool := make_tool(w)
	var hit: Dictionary = w.hooks().debug_raycast(Vector3(40, 80, 40), Vector3(0, -1, 0))
	assert_bool(hit["hit"]).is_true()
	var hp: Vector3 = hit["pos"]
	var before: Color = w.hooks().debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(before.r < 0.52 and before.g > 0.05).is_true() # solid terrain

	var r: Dictionary = tool.apply_sphere_subtract(Vector3(hp.x, hp.y + 0.5, hp.z), 2.5)
	assert_array(r["rejected"]).is_empty()

	# The crater is real (deeper hit, still terrain below) and no brick was ever dropped.
	for i in range(10):
		w.hooks().debug_stream_frame(CAM)
	var after: Dictionary = w.hooks().debug_raycast(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_float(after["pos"].y).is_less(hp.y - 1.0)
	var c: Color = w.hooks().debug_raymarch_pixel(Vector3(hp.x, hp.y + 2.0, hp.z), Vector3(0, -1, 0))
	assert_bool(c.r < 0.52 and c.g > 0.05).is_true()
	assert_int(w.hooks().debug_stream_stats()["overflow_ever"]).is_equal(0)
