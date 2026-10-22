extends SceneTree
# Writes water's atlas maps, assets/materials/08_*.png (docs/superpowers/specs/2026-10-06-
# water-voxels-design.md §2). Run once from the repo root; the outputs are committed:
#   godot --headless --path . -s res://tools/make_water_textures.gd
#
# 08_normal.png is Filter Forge's seamless water normal map, used as is (steep and choppy on
# purpose); download it and pass its path after --:
#   curl -o /tmp/4141-normal.jpg https://www.filterforge.com/filters/4141-normal.jpg
#   godot --headless --path . -s res://tools/make_water_textures.gd -- /tmp/4141-normal.jpg
# The liquid shading path samples only the normal; the other maps are flat so MaterialAtlas
# has every map it requires (it refuses to load a layer with one missing) and
# transparency-off water draws in its scatter colour.

const SIZE := 512

func _initialize() -> void:
	var dir := ProjectSettings.globalize_path("res://assets/materials/")
	_flat(Color(0.03, 0.16, 0.20)).save_png(dir + "08_basecolor.png")
	_flat(Color(0.06, 0.06, 0.06)).save_png(dir + "08_roughness.png")
	_flat(Color(1.0, 1.0, 1.0)).save_png(dir + "08_ambientOcclusion.png")
	_flat(Color(0.0, 0.0, 0.0)).save_png(dir + "08_height.png")
	_normal(OS.get_cmdline_user_args()[0]).save_png(dir + "08_normal.png")
	print("make_water_textures: wrote 08_*.png to %s" % dir)
	quit()

func _flat(c: Color) -> Image:
	var img := Image.create_empty(SIZE, SIZE, false, Image.FORMAT_RGB8)
	img.fill(c)
	return img

func _normal(path: String) -> Image:
	var img := Image.load_from_file(path)
	img.convert(Image.FORMAT_RGB8)
	img.resize(SIZE, SIZE, Image.INTERPOLATE_LANCZOS)
	return img
