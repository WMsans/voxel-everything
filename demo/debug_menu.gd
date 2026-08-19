extends PanelContainer

@export var world_path: NodePath

const EFFECTS := [
	["SSGI", "ssgi"],
	["SSR", "ssr"],
	["Contact shadows", "contact_shadows"],
	["Outlines", "outlines"],
	["Sun shadow map", "sun_shadow_map"],
	["Glossy SDF rays", "glossy_sdf_rays"],
	["Raymarched sun shadow", "raymarched_sun_shadow"],
]

var _world: VoxelWorld
var _quality: OptionButton
var _checks: Dictionary = {}
var _syncing := false

func _ready() -> void:
	_world = get_node_or_null(world_path) as VoxelWorld
	visible = false
	var box := VBoxContainer.new()
	box.name = "Controls"
	add_child(box)
	var title := Label.new()
	title.text = "Beauty (F1)"
	box.add_child(title)
	_quality = OptionButton.new()
	_quality.name = "Quality"
	for label in ["Off", "Low", "Medium", "High"]:
		_quality.add_item(label)
	_quality.item_selected.connect(_on_quality)
	box.add_child(_quality)
	for entry in EFFECTS:
		var check := CheckBox.new()
		check.name = entry[1]
		check.text = entry[0]
		check.toggled.connect(_on_effect.bind(entry[1]))
		_checks[entry[1]] = check
		box.add_child(check)
	_sync()

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F1:
		visible = not visible
		if visible:
			_sync()
		get_viewport().set_input_as_handled()

func _on_quality(index: int) -> void:
	if _syncing or not _world:
		return
	_world.quality_tier = index
	_sync()

func _on_effect(on: bool, effect: String) -> void:
	if _syncing or not _world:
		return
	_world.set_effect_enabled(effect, on)
	_sync()

func _sync() -> void:
	if not _world:
		return
	_syncing = true
	var settings: Dictionary = _world.debug_beauty_settings()
	_quality.select(int(settings["tier"]))
	for entry in EFFECTS:
		(_checks[entry[1]] as CheckBox).set_pressed_no_signal(bool(settings[entry[1]]))
	_syncing = false
