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
}

# Recorded 2026-09-13 on Apple M1; run-to-run spread 0.0, tolerance = max(3x spread, 0.004).
const GOLDEN := {"down_close":{"hiz_built":true,"tiles":[0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196],"two_phase":true},"horizon":{"hiz_built":true,"tiles":[0.283769,0.256863,0.252396,0.25177,0.273475,0.280501,0.300463,0.301906,0.296977,0.294907,0.283006,0.284831,0.284885,0.267919,0.255065,0.249428,0.298556,0.293246,0.301906,0.28818,0.288752,0.2875,0.287581,0.29153,0.301688,0.300953,0.30196,0.30196,0.30177,0.30196,0.29665,0.290877,0.300435,0.301361,0.30196,0.30196,0.30196,0.30196,0.30196,0.300544,0.300653,0.300844,0.301307,0.30196,0.30196,0.30196,0.30196,0.30196,0.30098,0.301416,0.30147,0.300925,0.301634,0.301906,0.30098,0.301062,0.30196,0.301361,0.300844,0.30196,0.301307,0.301143,0.301851,0.301742],"two_phase":true},"oblique":{"hiz_built":true,"tiles":[0.297412,0.294607,0.299101,0.287636,0.298066,0.287881,0.285321,0.293709,0.301933,0.301443,0.30196,0.300708,0.295561,0.30196,0.295043,0.293763,0.300653,0.301089,0.30196,0.30196,0.30196,0.30196,0.30196,0.297031,0.299918,0.301143,0.301742,0.30196,0.30196,0.30196,0.30196,0.30196,0.300326,0.30128,0.301062,0.301143,0.30196,0.30196,0.301497,0.300817,0.30196,0.301171,0.301143,0.301688,0.301034,0.301116,0.301388,0.301552,0.30196,0.30196,0.30177,0.300817,0.301661,0.301824,0.300925,0.30128,0.30196,0.30196,0.30196,0.30196,0.30147,0.30147,0.301906,0.30196],"two_phase":true}}
const TOL_TILE := 0.004

var _nodes: Array = []

func after_test() -> void:
	for n in _nodes:
		if is_instance_valid(n):
			n.free()
	_nodes.clear()

func make_scene() -> Dictionary:
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.use_local_device = false
	world.physics_enabled = false
	add_child(world)
	_nodes.append(world)
	# The far field needs a MeshService; without it this golden would not cover the LoD stage.
	assert_bool(world.hooks().debug_init_physics()).is_true()
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
