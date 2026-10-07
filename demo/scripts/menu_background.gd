extends TextureRect
# The title and Create New World screens tile a ground albedo behind their buttons (spec 6.1).
# assets/materials/ carries a .gdignore -- the native atlas reads those PNGs straight off disk --
# so Godot never imports them and a scene-authored Texture2D resource cannot load. Read the file
# straight off disk instead, the same way demo/scripts/material_picker.gd's _swatch() does.

@export_file("*.png") var texture_path := "res://assets/materials/02_basecolor.png"

func _ready() -> void:
	if texture:
		return
	var img := Image.load_from_file(ProjectSettings.globalize_path(texture_path))
	if img:
		texture = ImageTexture.create_from_image(img)
