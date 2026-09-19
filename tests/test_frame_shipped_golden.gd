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

# Re-recorded 2026-09-19 on Apple M1: the leaf raster draws cards in the shipped frame
# (Task 12), so canopies are new pixels -- oblique tile 6 moved 0.0076 and horizon tile 9
# moved 0.0157 against the grass-era numbers below; down_close is unchanged. Same policy as
# the Task-7 grass re-record: intentional change, re-recorded in the commit that causes it.
# Re-recorded 2026-09-18 on Apple M1: near-field grass now covers the whole resident sphere
# rather than a 10 m slab around the camera, and these cameras all sit 14 m above the ground,
# so every tile with ground in it moved. Recorded with the gust frozen (see freeze_grass
# below); run-to-run spread is 0.0 again, so the tolerance stays where it was.
const GOLDEN := {"down_close":{"hiz_built":true,"tiles":[0.30196,0.30196,0.30196,0.298529,0.276498,0.287064,0.297576,0.30196,0.30196,0.30196,0.30196,0.30196,0.298148,0.298502,0.30147,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.30196,0.301879,0.301416,0.299019,0.30196,0.30196,0.30196,0.30196,0.30196,0.283987,0.270098,0.28799,0.296459],"two_phase":true},"horizon":{"hiz_built":true,"tiles":[0.283769,0.254057,0.246895,0.249019,0.27244,0.280419,0.298284,0.301906,0.295724,0.275381,0.282843,0.284749,0.275136,0.254466,0.248611,0.242647,0.298556,0.2872,0.301906,0.28818,0.286329,0.281617,0.282788,0.276307,0.301688,0.300953,0.30196,0.30196,0.298229,0.299836,0.296514,0.286138,0.300326,0.301361,0.30196,0.30196,0.30196,0.301879,0.30196,0.301089,0.300599,0.300708,0.301307,0.301552,0.30196,0.30196,0.30196,0.301579,0.30098,0.301416,0.30147,0.295207,0.2994,0.300408,0.29872,0.300925,0.30196,0.301361,0.300844,0.298611,0.279874,0.285376,0.297739,0.301742],"two_phase":true},"oblique":{"hiz_built":true,"tiles":[0.297195,0.281209,0.299101,0.287636,0.297903,0.277696,0.275898,0.286764,0.301933,0.301443,0.30196,0.300708,0.295043,0.29842,0.294907,0.287064,0.300626,0.301089,0.30196,0.30196,0.30196,0.301824,0.30196,0.29763,0.299863,0.301143,0.301742,0.30196,0.30196,0.30196,0.30196,0.301715,0.300326,0.30128,0.301062,0.299564,0.300626,0.30196,0.301007,0.300708,0.30196,0.301171,0.301143,0.297358,0.278567,0.281018,0.299264,0.301552,0.30196,0.30196,0.30177,0.300571,0.298175,0.299074,0.300817,0.30128,0.30196,0.30196,0.30196,0.30196,0.30147,0.30147,0.301906,0.30196],"two_phase":true}}
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
