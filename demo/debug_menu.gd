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
	["SSAO", "ssao"],
	["Islands", "islands"],
]

# The magnitude knobs, as [label, name, min, max]. These are what the emissive-crack look is
# actually tuned with, and until this work three of them were literals inside SsgiPass::render
# with no way to move them at runtime at all.
const VALUES := [
	["Emissive light", "emissive_gi_strength", 0.0, 64.0],
	["Emissive reach (m)", "emissive_gi_radius", 0.25, 128.0],
	["GI bounce", "ssgi_strength", 0.0, 8.0],
	["GI reach (m)", "ssgi_radius", 0.25, 64.0],
]

# The grass knobs, as [label, name, min, max, step]. Same row pattern as EFFECTS/VALUES
# above, but wired to set_grass_value/get_grass_value and carrying an explicit per-row
# step (the VALUES step heuristic of range/128 would give unusable increments for the
# sub-metre blade knobs).
const GRASS := [
	["Grass", "enabled"],
]

const GRASS_VALUES := [
	["Density", "blades_per_brick", 0.0, 64.0, 1.0],
	["Reach", "reach_m", 0.0, 120.0, 1.0],
	["Blade width", "blade_width_m", 0.0, 0.06, 0.002],
	["Blade height", "blade_height_m", 0.0, 1.5, 0.05],
	["Wind strength", "wind_strength", 0.0, 1.5, 0.05],
	["Wind speed", "wind_speed", 0.0, 3.0, 0.1],
]

var _world: VoxelWorld
var _quality: OptionButton
var _checks: Dictionary = {}
var _sliders: Dictionary = {}
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
	for entry in VALUES:
		var label := Label.new()
		label.name = entry[1] + "_label"
		label.text = entry[0]
		box.add_child(label)
		var slider := HSlider.new()
		slider.name = entry[1]
		slider.min_value = entry[2]
		slider.max_value = entry[3]
		# The knobs span two orders of magnitude between them, so a fixed step would be
		# either too coarse for the bounce or uselessly fine for the reach.
		slider.step = (entry[3] - entry[2]) / 128.0
		slider.value_changed.connect(_on_value.bind(entry[1]))
		_sliders[entry[1]] = slider
		box.add_child(slider)
	var grass_title := Label.new()
	grass_title.name = "Grass_section"
	grass_title.text = "Grass"
	box.add_child(grass_title)
	for entry in GRASS:
		var check := CheckBox.new()
		check.name = entry[1]
		check.text = entry[0]
		check.toggled.connect(_on_grass_effect.bind(entry[1]))
		_checks[entry[1]] = check
		box.add_child(check)
	for entry in GRASS_VALUES:
		var glabel := Label.new()
		glabel.name = entry[1] + "_label"
		glabel.text = entry[0]
		box.add_child(glabel)
		var gslider := HSlider.new()
		gslider.name = entry[1]
		gslider.min_value = entry[2]
		gslider.max_value = entry[3]
		gslider.step = entry[4]
		gslider.value_changed.connect(_on_grass_value.bind(entry[1]))
		_sliders[entry[1]] = gslider
		box.add_child(gslider)
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

func _on_value(value: float, knob: String) -> void:
	if _syncing or not _world:
		return
	_world.set_effect_value(knob, value)
	# Deliberately no _sync() here: the settings are clamped on the way in, and rewriting the
	# slider from the clamped value mid-drag fights the pointer.

func _on_grass_effect(on: bool, knob: String) -> void:
	if _syncing or not _world:
		return
	_world.set_grass_value(knob, 1.0 if on else 0.0)
	_sync()

func _on_grass_value(value: float, knob: String) -> void:
	if _syncing or not _world:
		return
	_world.set_grass_value(knob, value)
	# Deliberately no _sync() here, same as _on_value: the settings are clamped on the way
	# in, and rewriting the slider from the clamped value mid-drag fights the pointer.

func _sync() -> void:
	if not _world:
		return
	_syncing = true
	var settings: Dictionary = _world.hooks().debug_beauty_settings()
	_quality.select(int(settings["tier"]))
	for entry in EFFECTS:
		(_checks[entry[1]] as CheckBox).set_pressed_no_signal(bool(settings[entry[1]]))
	for entry in VALUES:
		(_sliders[entry[1]] as HSlider).set_value_no_signal(float(settings[entry[1]]))
	for entry in GRASS:
		(_checks[entry[1]] as CheckBox).set_pressed_no_signal(_world.get_grass_value(entry[1]) != 0.0)
	for entry in GRASS_VALUES:
		(_sliders[entry[1]] as HSlider).set_value_no_signal(float(_world.get_grass_value(entry[1])))
	_syncing = false
