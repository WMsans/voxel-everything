extends SceneTree
# Writes water's atlas maps, assets/materials/08_*.png (docs/superpowers/specs/2026-10-06-
# water-voxels-design.md §2). Run once from the repo root; the outputs are committed:
#   godot --headless --path . -s res://tools/make_water_textures.gd
#
# 08_normal.png is a TILEABLE ripple normal map: a sum of sines whose wave vectors are whole
# numbers of cycles per tile, so it wraps seamlessly by construction. The liquid shading path
# samples only the normal; the other maps are flat so MaterialAtlas has every map it requires
# (it refuses to load a layer with one missing) and transparency-off water draws in its
# scatter colour.

const SIZE := 512
# [cycles_x, cycles_y, amplitude, phase]. Integer cycles keep the sum periodic on the tile;
# mixed directions and falling amplitudes keep it from reading as a grid.
const WAVES := [
	[3, 1, 1.00, 0.0], [-2, 3, 0.80, 1.3], [5, -2, 0.55, 2.1], [1, 6, 0.45, 0.4],
	[-7, -3, 0.30, 4.0], [9, 4, 0.22, 5.2], [-4, 11, 0.16, 0.9], [13, -6, 0.11, 3.3],
]
# Tangent-space slope at the steepest texel. The shader scales it again by wave_strength.
const SLOPE := 0.35

func _initialize() -> void:
	var dir := ProjectSettings.globalize_path("res://assets/materials/")
	_flat(Color(0.03, 0.16, 0.20)).save_png(dir + "08_basecolor.png")
	_flat(Color(0.06, 0.06, 0.06)).save_png(dir + "08_roughness.png")
	_flat(Color(1.0, 1.0, 1.0)).save_png(dir + "08_ambientOcclusion.png")
	_flat(Color(0.0, 0.0, 0.0)).save_png(dir + "08_height.png")
	_normal().save_png(dir + "08_normal.png")
	print("make_water_textures: wrote 08_*.png to %s" % dir)
	quit()

func _flat(c: Color) -> Image:
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGB8)
	img.fill(c)
	return img

func _normal() -> Image:
	var gx := PackedFloat32Array()
	var gy := PackedFloat32Array()
	gx.resize(SIZE * SIZE)
	gy.resize(SIZE * SIZE)
	var peak := 0.0
	for y in range(SIZE):
		for x in range(SIZE):
			var dx := 0.0
			var dy := 0.0
			for w in WAVES:
				var kx: float = TAU * float(w[0])
				var ky: float = TAU * float(w[1])
				var c: float = float(w[2]) * cos(kx * x / SIZE + ky * y / SIZE + float(w[3]))
				dx += c * kx # d/du of amplitude * sin(phase), u in [0, 1)
				dy += c * ky
			var i := y * SIZE + x
			gx[i] = dx
			gy[i] = dy
			peak = maxf(peak, sqrt(dx * dx + dy * dy))
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGB8)
	for y in range(SIZE):
		for x in range(SIZE):
			var i := y * SIZE + x
			var n := Vector3(-gx[i] / peak * SLOPE, -gy[i] / peak * SLOPE, 1.0).normalized()
			img.set_pixel(x, y, Color(n.x * 0.5 + 0.5, n.y * 0.5 + 0.5, n.z * 0.5 + 0.5))
	return img
