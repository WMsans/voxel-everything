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

# Re-recorded 2026-09-21 on Apple M1: far canopies. The leaf scatter used to cull every tree
# whose trunk anchor was not in the brick atlas, which is resident for only ~60 m, so canopies
# past that were bare trunks; it now trusts the analytic tree_at() where the REGION is not
# resident, and leaf reach_m went 250 -> 800. Clean main passed this suite immediately before
# the re-record. Moved: grove tile 33 by 0.005981, horizon tile 19 by 0.003942, oblique tile
# 4 by 0.00273 -- canopy tiles only; down_close is bit-exact. Two runs of the re-recorded
# numbers spread at most 0.000844 (horizon), 4.7x under TOL_TILE. Same policy: intentional
# change, re-recorded in the commit that causes it.
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
# Re-recorded 2026-10-01 on Apple M1: with the canopy gust frozen as well as the grass (see
# freeze_grass). The trees-stage early-outs made streaming several times faster, so settle()
# now returns hundreds of frames earlier and the leaf gust phase at capture moved with it:
# horizon tile 28 by 0.006672 against the unfrozen golden. That was phase, not picture --
# with the gust frozen, the frame before and after the early-outs agrees on every tile of
# all four cameras to the sixth decimal, which is the measurement these numbers come from.
# Re-recorded 2026-10-06 on an Intel Iris Xe (Linux/Vulkan): the far-field LoD target went
# from 3 to 6 px per cell (ve::kLodTargetCellPx) and the near-dense radius from a fixed 300 m
# to the measured fade band's end, so distant terrain is drawn coarser by design. Against the
# M1 golden: horizon tile 5 by 0.007707, grove tile 32 by 0.003486, down_close tile 61 by
# 0.001689, oblique tile 4 by 0.000844. Two runs of the re-recorded numbers agree on every
# tile of all four cameras (spread 0). The same branch's culling and decode changes alone
# passed this suite against the M1 golden. Intentional change, re-recorded in the commit
# that causes it.
# Re-recorded 2026-10-18: HBAO + the spiral SSGI gather became one visibility-bitmask horizon
# pass, and SSR is off by default (60 fps steady budget). 22 tiles moved past TOL_TILE, mostly
# darker -- the bitmask AO is stronger in creases -- with some brighter from the bounce, which
# no longer divides by 1 + d^2. Worst: grove tile 48 -0.016067, oblique tile 11 -0.014787,
# horizon tile 14 -0.011602, down_close tile 5 -0.010403. Two runs of the re-recorded numbers
# were bit-identical. Same policy: intentional change, re-recorded in the commit that causes it.
const GOLDEN := {"down_close":{"hiz_built":true,"tiles":[0.30196,0.30196,0.30196,0.298556,0.277941,0.276661,0.297521,0.30196,0.30196,0.30196,0.30196,0.30196,0.295152,0.299809,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.301198,0.30019,0.30196,0.30196,0.30196,0.30196,0.30196,0.285376,0.272631,0.286084,0.298338],"two_phase":true},"grove":{"hiz_built":true,"tiles":[0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.301388,0.294526,0.28799,0.287309,0.29025,0.30196,0.275,0.262473,0.264924,0.293436,0.29025,0.288072,0.30196,0.30196,0.300817,0.30196,0.297875,0.283033,0.283632,0.299891,0.30196,0.30196,0.267429,0.271786,0.280664,0.275054,0.287663,0.301824,0.30196,0.30196,0.280882,0.276498,0.290632,0.285049,0.290686,0.297331,0.30196,0.30196],"two_phase":true},"horizon":{"hiz_built":true,"tiles":[0.278431,0.24951,0.243873,0.250899,0.270425,0.286792,0.299945,0.30196,0.298502,0.29202,0.275681,0.281944,0.257516,0.258524,0.242592,0.241884,0.298992,0.291857,0.30196,0.273611,0.266884,0.281427,0.282843,0.287554,0.30196,0.299564,0.30196,0.273066,0.274591,0.30196,0.296677,0.28976,0.30196,0.30196,0.30196,0.29793,0.278486,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.301198,0.293436,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.295261,0.298665,0.299809,0.297576,0.30196,0.30196,0.30196,0.30196,0.296841,0.279711,0.282162,0.297658,0.30196],"two_phase":true},"oblique":{"hiz_built":true,"tiles":[0.295724,0.292538,0.298393,0.276144,0.266285,0.285294,0.280038,0.291884,0.30196,0.30196,0.30196,0.269063,0.272358,0.30196,0.294226,0.29134,0.30196,0.30196,0.30196,0.284722,0.277451,0.30196,0.30196,0.297549,0.300136,0.30196,0.30196,0.30196,0.282162,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.295179,0.300762,0.30196,0.300435,0.30196,0.30196,0.30196,0.30196,0.299673,0.27805,0.282788,0.299673,0.30196,0.30196,0.30196,0.30196,0.30196,0.299428,0.298611,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196],"two_phase":true}}
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
	# The canopy sways on the same gust field and the same frame counter, and settle() does
	# not take a fixed number of frames: anything that changes streaming speed changes the
	# phase at capture, and that moved a canopy tile past TOL_TILE with no change to the image.
	world.set_leaf_value("wind_speed", 0.0)
	world.set_leaf_value("wind_strength", 0.0)

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
