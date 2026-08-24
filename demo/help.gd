extends Control
# Help overlay for the demo shell. It is built from a single CONTROLS table so the text
# cannot drift from the bindings the demo actually handles.

const CONTROLS := [
	["Move", "W A S D"],
	["Fly up / down", "E / Q  (hold Shift to boost)"],
	["Fly / walk", "F"],
	["Jump", "Space"],
	["Fire tool", "Left mouse"],
	["Select tool", "1 Carve   2 Fill   3 Paint   4 Drill"],
	["Tool radius", "Mouse Wheel"],
	["Drill (shortcut)", "R"],
	["Choose material", "M"],
	["Beauty menu", "F1"],
	["This help", "F2"],
	["Raymarch cost view", "F3"],
	["HUD detail", "F4"],
	["Reload shaders", "F5"],
	["Run self-check", "F6"],
	["Graphics settings", "F7"],
	["Screenshot", "F12"],
	["Pause", "P"],
	["Release mouse", "Esc"],
]

const AUTO_HIDE_SECONDS := 8.0

var _elapsed := 0.0

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	mouse_filter = Control.MOUSE_FILTER_IGNORE
	visible = true
	_elapsed = 0.0
	_build_ui()

func help_text() -> String:
	var lines := PackedStringArray(["Voxel Everything — controls", ""])
	for row in CONTROLS:
		lines.append("%-20s %s" % [row[0], row[1]])
	return "\n".join(lines)

func _process(delta: float) -> void:
	if not visible:
		return
	_elapsed += delta
	if _elapsed >= AUTO_HIDE_SECONDS:
		visible = false

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F2:
		visible = not visible
		if visible:
			_elapsed = 0.0
		get_viewport().set_input_as_handled()

func _build_ui() -> void:
	var center := CenterContainer.new()
	center.name = "Center"
	center.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	center.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(center)
	var panel := PanelContainer.new()
	panel.name = "Panel"
	panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
	center.add_child(panel)
	var label := Label.new()
	label.name = "Text"
	label.text = help_text()
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	panel.add_child(label)
