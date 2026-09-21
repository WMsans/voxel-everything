extends GdUnitTestSuite

# Characterization, not specification. This is the ONLY suite that renders through the real
# compositors (main RenderingDevice, SubViewport, Compositor on the camera) instead of a
# local-device hook. It pins what the shipped frame produced before the frame module moved
# the compositor bodies (docs/superpowers/plans/2026-09-13-frame-module.md, Task 2), so
# "the move changed nothing" is a measurement. If an intentional change moves these numbers,
# re-record them in the same commit and say why in the message.
#
# Measured quantity: mean luma of each tile in an 8x8 grid over the tonemapped viewport,
# averaged over AVERAGE_FRAMES consecutive frames so SSGI's temporal jitter averages out,
# plus the two booleans of the LoD cull record. GPU timing values are invalid on this machine
# and are deliberately not pinned.

const W := 256
const H := 144
const TILES := 8
const SETTLE_FRAMES := 240
const LOD_QUIET_FRAMES := 30
const LOD_SETTLE_BUDGET := 3000
const AVERAGE_FRAMES := 8
const SAMPLE_STEP := 2
const CAMERAS := {
	"down_close": [Vector3(30.0, 70.0, 30.0), Vector3(0.2, -1.0, 0.2)],
	"oblique": [Vector3(30.0, 70.0, 30.0), Vector3(0.5, -0.5, 0.5)],
	"horizon": [Vector3(30.0, 70.0, 30.0), Vector3(0.35, -0.2, 0.35)],
	# Task 14: the grove view from tools/leaf_capture.gd -- (20, 60, 30) is the stream
	# origin tests/test_leaves.gd uses (it documented WHY: the usual hook view sits inside
	# the cave), and the direction is the capture's default, so this camera is the machine-
	# checked half of the leaf golden; the capture's PNG is the human-readable half.
	"grove": [Vector3(20.0, 60.0, 30.0), Vector3(0.6, 0.05, -1.0)],
}

# Re-recorded 2026-09-21 on Apple M1: the fluffy-canopy pass. The crown grew (trees stage
# crown_radius 4.5 -> 6.0, and tree_lobe now drapes its odd lobes below the crown centre
# instead of piling every lobe above it), the card became a sprig of leaves rather than one
# blob (leaf.frag.glsl's cellular grain, LeafSettings::leaf_grain), the cards grew to pay
# for both (clump_radius_m 0.85 -> 1.5) and outline.comp.glsl no longer draws a line between
# two canopy cards. Limb tips move with the lobes, so BARK voxels move and the terrain field
# goldens moved with them in the same commit. Clean main passed this suite immediately
# before the re-record, so the movement is this change and nothing else: oblique tile 19 by
# 0.014134 and horizon tile 27 by 0.024053 against TOL_TILE 0.004. Two runs of the
# re-recorded numbers spread at most 0.000602 (horizon tile 20), still 6.6x under TOL_TILE.
# Same policy as every re-record below: intentional change, re-recorded in the commit that
# causes it.
# Re-recorded 2026-09-19 on Apple M1: leaf clumps now carry their own attachment probe
# (shaders/leaf_scatter.comp.glsl), so a clump whose nearest skeleton wood is absent from
# the live atlas is not placed -- the canopies thin by ~18% of their clumps and hug the
# limbs instead of filling the lobe shells. 60 tiles moved (26 brighter, 34 darker: thinner
# canopy shows both sky and shaded bark through), 26 past TOL_TILE -- worst oblique tile 4
# at -0.020241, horizon tile 36 at -0.017756, grove tile 27 at +0.018099. down_close is
# bit-identical (no canopy pixels). Two runs of the re-recorded numbers spread at most
# 0.000436 (horizon), still an order under TOL_TILE. Same policy as the Task-12 re-record
# below: intentional change, re-recorded in the commit that causes it.
# Re-recorded 2026-09-19 on Apple M1: the spec §4 height band (final review wave) removed
# every tree the old gate had placed outside the grass band, so dark bark/canopy pixels in
# these frames revert to the bare surface. 59 tiles moved: 55 brighter, 4 marginally
# darker (grove 19/32, horizon 5, oblique 7, all <= 0.0005 -- removing canopies moves
# SSAO and lighting in both directions); 14 exceeded TOL_TILE (oblique 1/5, horizon 2/9/12/13/17/21,
# grove 14/22/23/27/28/36), so the per-camera worst-tile asserts failed on oblique tile 1
# (0.014161), horizon tile 9 (0.019642) and grove tile 22 (0.018137), and 24 more moved
# above 5e-4 under tolerance; down_close is unchanged. Recorded from the full-suite run
# that produced the failure, so the numbers are the shipped band code's output; the 59/14/
# 24 counts and the three cited deltas were re-derived from the two GOLDEN versions in
# review. Same policy: intentional change, re-recorded in the commit that causes it.
# Re-recorded 2026-09-19 on Apple M1: canopy sway on the shared gust field (Task 13) moves
# the canopies in these frames -- the largest tile move is horizon tile 15 at 0.002179, with
# oblique/horizon canopy tiles moving 0.0001..0.0008; down_close is bit-identical (no canopy
# pixels). The gust phase rides the frame counter, and settle() can take a frame more or
# less, so these tiles now carry a run-to-run spread of ~0.00006 -- two orders under
# TOL_TILE, which stays 0.004. Same policy as the Task-12 re-record below: intentional
# change, re-recorded in the commit that causes it.
# Re-recorded 2026-09-19 on Apple M1: the leaf raster draws cards in the shipped frame
# (Task 12), so canopies are new pixels -- oblique tile 6 moved 0.0076 and horizon tile 9
# moved 0.0157 against the grass-era numbers below; down_close is unchanged. Same policy as
# the Task-7 grass re-record: intentional change, re-recorded in the commit that causes it.
# Re-recorded 2026-09-18 on Apple M1: near-field grass now covers the whole resident sphere
# rather than a 10 m slab around the camera, and these cameras all sit 14 m above the ground,
# so every tile with ground in it moved. Recorded with the gust frozen (see freeze_grass
# below); run-to-run spread is 0.0 again, so the tolerance stays where it was.
#
# Added 2026-09-19 (Task 14): the grove camera and its numbers, recorded on Apple M1. The
# three pre-existing cameras were not re-recorded.
# Grove margins, measured 2026-09-19 (Task 15): two independent full-suite runs, per-tile
# |measured - GOLDEN| below. Run A: nonzero only at tiles 15 (0.000102), 19 (0.000041),
# 20 (0.000007), 27 (0.000272); run B: tiles 14 (0.000068), 18 (0.000068), 26 (0.000096),
# 27 (0.000170), 19 (0.000221) and 28 (0.000381 = worst); every other canopy tile and all
# ground tiles bit-exact both runs. Worst margin vs TOL_TILE is still 10x, the movers are
# the canopy tiles the gust phase above describes, and the spread does NOT grow toward the
# tolerance -- re-record only on an intentional change, never to chase these.
const GOLDEN := {"down_close":{"hiz_built":true,"tiles":[0.30196,0.30196,0.30196,0.298529,0.276498,0.287064,0.297576,0.30196,0.30196,0.30196,0.30196,0.30196,0.298148,0.298502,0.30147,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.301879,0.301416,0.299019,0.30196,0.30196,0.30196,0.30196,0.30196,0.283987,0.270098,0.28799,0.296459],"two_phase":true},"grove":{"hiz_built":true,"tiles":[0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.301824,0.29586,0.288072,0.288807,0.292456,0.30196,0.282026,0.273257,0.271568,0.293573,0.29183,0.290277,0.30196,0.30196,0.301797,0.301525,0.298556,0.28513,0.284313,0.300762,0.30196,0.30196,0.283578,0.278703,0.281971,0.280909,0.290141,0.30196,0.30196,0.30196,0.283878,0.282598,0.289842,0.288562,0.289869,0.298066,0.30196,0.30196],"two_phase":true},"horizon":{"hiz_built":true,"tiles":[0.283769,0.256863,0.252396,0.251579,0.273475,0.280501,0.300463,0.301906,0.296977,0.294907,0.283006,0.283932,0.264396,0.267919,0.255065,0.248175,0.298556,0.293246,0.301906,0.278213,0.268886,0.2875,0.287581,0.290849,0.301688,0.300953,0.30196,0.274319,0.278751,0.30196,0.29665,0.291775,0.300326,0.301361,0.301933,0.299918,0.281263,0.30177,0.30196,0.301089,0.300599,0.300844,0.300925,0.298747,0.291258,0.301062,0.30196,0.301579,0.30098,0.301416,0.30147,0.29439,0.299755,0.301034,0.299264,0.300925,0.30196,0.301361,0.300081,0.298284,0.279629,0.285593,0.297875,0.301742],"two_phase":true},"oblique":{"hiz_built":true,"tiles":[0.297412,0.294607,0.299101,0.276872,0.270176,0.287881,0.285321,0.293709,0.301933,0.301443,0.30196,0.28189,0.275456,0.30196,0.295043,0.293464,0.300626,0.301089,0.30196,0.286635,0.275762,0.30196,0.30196,0.29763,0.299863,0.301143,0.301579,0.301143,0.283728,0.301334,0.30196,0.301715,0.300326,0.30128,0.301034,0.297344,0.299605,0.301416,0.301007,0.300708,0.30196,0.301171,0.300735,0.297195,0.278595,0.28095,0.299646,0.301552,0.30196,0.30196,0.300626,0.300354,0.298243,0.29891,0.300817,0.30128,0.30196,0.30196,0.30196,0.30196,0.30147,0.30147,0.301906,0.30196],"two_phase":true}}
const TOL_TILE := 0.004

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

# Grass covers the whole resident sphere now, so all three of these cameras have blades in
# frame -- and a blade's LEAN follows the gust field (grass.vert.glsl), which scrolls with the
# frame counter whatever the wind strength is. Eight frames do not average that out: it moved
# the run-to-run tile spread from 0.0 to 0.0016 here and to 0.008 in the reload contract, which
# is characterising the wind's phase at capture time rather than the frame. Stopping the scroll
# makes the picture a function of the world and the camera again, the same fix
# test_frame_contract's determinism case needs. Shared with that contract, which compares
# against this suite's golden.
static func freeze_grass(world: VoxelWorld) -> void:
	world.set_grass_value("wind_speed", 0.0)
	world.set_grass_value("wind_strength", 0.0)

func make_scene() -> Dictionary:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = false
	world.physics_enabled = false
	add_child(world)
	_nodes.append(world)
	# The far field needs a MeshService; without it this golden would not cover the LoD stage.
	assert_bool(world.hooks().debug_init_physics()).is_true()
	freeze_grass(world)
	var raymarch: RaymarchCompositor = ClassDB.instantiate("RaymarchCompositor")
	raymarch.world_path = world.get_path()
	var beauty: BeautyCompositor = ClassDB.instantiate("BeautyCompositor")
	beauty.world_path = world.get_path()
	var effects: Array[CompositorEffect] = [raymarch, beauty]
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	var vp := SubViewport.new()
	vp.size = Vector2i(W, H)
	vp.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child(vp)
	_nodes.append(vp)
	var cam := Camera3D.new()
	cam.fov = 60.0
	cam.near = 0.05
	cam.far = 4000.0
	cam.compositor = compositor
	vp.add_child(cam)
	cam.current = true
	return {"world": world, "viewport": vp, "camera": cam}

func wait_frames(n: int) -> void:
	for i in range(n):
		await get_tree().process_frame

func settle(world: VoxelWorld) -> bool:
	await wait_frames(SETTLE_FRAMES)
	var quiet := 0
	for i in range(LOD_SETTLE_BUDGET):
		await get_tree().process_frame
		var s: Dictionary = world.hooks().debug_lod_stats()
		var idle: bool = int(s.get("requests_pending", 1)) == 0 and int(s.get("builds_in_flight", 1)) == 0
		quiet = quiet + 1 if idle else 0
		if quiet >= LOD_QUIET_FRAMES:
			return true
	return false

func tile_luma(img: Image) -> PackedFloat32Array:
	var sums := PackedFloat32Array()
	var counts := PackedInt32Array()
	sums.resize(TILES * TILES)
	counts.resize(TILES * TILES)
	var w := img.get_width()
	var h := img.get_height()
	for y in range(0, h, SAMPLE_STEP):
		var ty := mini(y * TILES / h, TILES - 1)
		for x in range(0, w, SAMPLE_STEP):
			var tx := mini(x * TILES / w, TILES - 1)
			var c := img.get_pixel(x, y)
			sums[ty * TILES + tx] += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b
			counts[ty * TILES + tx] += 1
	for i in range(sums.size()):
		sums[i] = sums[i] / maxf(1.0, float(counts[i]))
	return sums

func measure_camera(s: Dictionary, key: String) -> Dictionary:
	var world: VoxelWorld = s["world"]
	var cam: Camera3D = s["camera"]
	var vp: SubViewport = s["viewport"]
	var pose: Array = CAMERAS[key]
	var pos: Vector3 = pose[0]
	var fwd: Vector3 = (pose[1] as Vector3).normalized()
	cam.global_position = pos
	cam.look_at(pos + fwd, Vector3.UP)
	assert_bool(await settle(world)).override_failure_message(
		"%s: the far field never settled" % key).is_true()
	var acc := PackedFloat32Array()
	acc.resize(TILES * TILES)
	for f in range(AVERAGE_FRAMES):
		await RenderingServer.frame_post_draw
		var t := tile_luma(vp.get_texture().get_image())
		for i in range(t.size()):
			acc[i] += t[i] / float(AVERAGE_FRAMES)
	var tiles: Array = []
	for v in acc:
		tiles.append(snappedf(v, 0.000001))
	var cull: Dictionary = world.hooks().debug_lod_cull_debug()
	return {"tiles": tiles, "two_phase": bool(cull["two_phase"]), "hiz_built": bool(cull["hiz_built"])}

func test_the_shipped_frame_matches_the_recorded_golden(timeout := 900000) -> void:
	var s := make_scene()
	var measured := {}
	for key in CAMERAS:
		measured[key] = await measure_camera(s, key)
	print("FRAME_GOLDEN ", JSON.stringify(measured))
	assert_bool(GOLDEN.is_empty()).override_failure_message(
		"no golden recorded: paste the FRAME_GOLDEN line above into GOLDEN").is_false()
	for key in CAMERAS:
		var got: Dictionary = measured[key]
		var want: Dictionary = GOLDEN[key]
		assert_bool(got["two_phase"]).override_failure_message(
			"%s: LoD two-phase decision moved" % key).is_equal(want["two_phase"])
		assert_bool(got["hiz_built"]).override_failure_message(
			"%s: HiZ build decision moved" % key).is_equal(want["hiz_built"])
		var worst := 0.0
		var worst_i := -1
		for i in range(TILES * TILES):
			var d := absf(float(got["tiles"][i]) - float(want["tiles"][i]))
			if d > worst:
				worst = d
				worst_i = i
		assert_float(worst).override_failure_message(
			"%s: tile %d moved by %.6f (tolerance %.6f)" % [key, worst_i, worst, TOL_TILE]
			).is_less_equal(TOL_TILE)
