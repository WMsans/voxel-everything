extends Control
# The title screen: one button, Create New World (spec §6.2).

const CREATE_WORLD := "res://demo/scenes/create_world.tscn"

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	var button := %CreateWorld as Button
	button.pressed.connect(func() -> void: get_tree().change_scene_to_file(CREATE_WORLD))
	button.grab_focus()
