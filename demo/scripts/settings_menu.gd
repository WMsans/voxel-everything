extends Control
# The settings panel (F1): window, render budget, beauty, grass and leaves dials in one place.
#
# It keeps no tables and no state of its own. Every tab is built from VoxelSettings.describe(), so a
# new row in a C++ settings table appears here with its label, range and hint, and a slider range
# cannot drift from the clamp behind it (S5). Every open re-reads the values, because the benchmark's
# flags, the dev keys and the inspector write the same stores. Persistence and the measured-run guard
# live in VoxelSettings; closing the panel asks it to save.
#
# One tab is not VoxelSettings': "Sun rays" is built from the knobs a SunRays effect exports (and
# its built-in enabled flag), read off the compositor of the WorldEnvironment at environment_path.
# The effect knows nothing of this menu. Its values persist in a [sun_rays] section of the same
# config file, written before VoxelSettings saves (it keeps sections it does not own).

@export var settings_path: NodePath
@export var environment_path: NodePath

const SUN_RAYS := "sun_rays"

var _settings: VoxelSettings
var _sun_rays: SunRays
var _controls := {}      # "group/name" -> the control for that row
var _value_labels := {}  # "group/name" -> the Label beside a slider
var _syncing := false
var _mouse_mode_before := Input.MOUSE_MODE_CAPTURED

@onready var _tabs: TabContainer = %Tabs

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_settings = get_node_or_null(settings_path) as VoxelSettings
	if _settings:
		for group in _settings.groups():
			_build_tab(group, _settings.describe(group))
	_sun_rays = _find_sun_rays()
	if _sun_rays:
		_load_sun_rays()
		_build_tab(SUN_RAYS, _sun_rays_rows())
	(%Reset as Button).pressed.connect(_on_reset)
	(%Close as Button).pressed.connect(_on_close)
	sync_from_settings()
	visible = false

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.keycode == KEY_F1:
		set_open(not visible)
		get_viewport().set_input_as_handled()
	elif visible and event.keycode == KEY_ESCAPE:
		# Closing beats player.gd's Esc (release the mouse): while this panel is up the mouse is
		# already free, and Esc reads as "dismiss the dialog".
		set_open(false)
		get_viewport().set_input_as_handled()

# Opening frees the cursor so the panel can be clicked; closing restores whatever mode was in force
# and persists what was set.
func set_open(open: bool) -> void:
	if open == visible:
		return
	visible = open
	if open:
		_mouse_mode_before = Input.mouse_mode
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		sync_from_settings()
	else:
		Input.mouse_mode = _mouse_mode_before
		_save()

func _exit_tree() -> void:
	# Quitting with the panel open still persists what was set in it.
	if visible:
		_save()

func _save() -> void:
	if _settings == null:
		return
	if _sun_rays:
		var cfg := ConfigFile.new()
		cfg.load(_settings.config_path)
		for row in _sun_rays_rows():
			cfg.set_value(SUN_RAYS, row["name"], _sun_rays.get(row["name"]))
		cfg.save(_settings.config_path)
	_settings.save()

func _find_sun_rays() -> SunRays:
	var env := get_node_or_null(environment_path) as WorldEnvironment
	if environment_path.is_empty() or env == null or env.compositor == null:
		return null
	for effect in env.compositor.compositor_effects:
		if effect is SunRays:
			return effect
	return null

# The same row shape VoxelSettings.describe() returns, so the tab is built and synced like the rest.
func _sun_rays_rows() -> Array:
	var rows := [{"name": "enabled", "label": "Enabled", "hint": "", "kind": "bool"}]
	for p in _sun_rays.get_property_list():
		if p["usage"] & PROPERTY_USAGE_SCRIPT_VARIABLE and p["hint"] == PROPERTY_HINT_RANGE:
			var r := String(p["hint_string"]).split(",")
			rows.append({"name": p["name"], "label": String(p["name"]).capitalize(), "hint": "",
				"kind": "float", "ui_min": float(r[0]), "ui_max": float(r[1]), "step": float(r[2])})
	return rows

# The file is hand-editable, so a value only lands on a knob the tab offers, inside its range.
func _load_sun_rays() -> void:
	var cfg := ConfigFile.new()
	if _settings == null or cfg.load(_settings.config_path) != OK:
		return
	for row in _sun_rays_rows():
		if not cfg.has_section_key(SUN_RAYS, row["name"]):
			continue
		var value = cfg.get_value(SUN_RAYS, row["name"])
		if row["kind"] == "bool":
			_sun_rays.set(row["name"], bool(value))
		else:
			_sun_rays.set(row["name"], clampf(float(value), row["ui_min"], row["ui_max"]))

func _read(group: String, name: String) -> Variant:
	if group == SUN_RAYS:
		return _sun_rays.get(name)
	return _settings.get_setting(group, name) if _settings else null

# The control for one row, or null.
func control(group: String, name: String) -> Control:
	return _controls.get(group + "/" + name)

func sync_from_settings() -> void:
	if _settings == null:
		return
	_syncing = true
	for key in _controls:
		var parts: PackedStringArray = String(key).split("/")
		var value = _read(parts[0], parts[1])
		var c: Control = _controls[key]
		if c is CheckBox:
			(c as CheckBox).set_pressed_no_signal(bool(value))
		elif c is HSlider:
			(c as HSlider).set_value_no_signal(float(value))
		elif c is OptionButton:
			var options := c as OptionButton
			options.select(options.get_item_index(int(value)))
		elif c is ColorPickerButton:
			(c as ColorPickerButton).color = value
		_update_value_label(parts[0], parts[1])
	if _controls.has("display/resolution") and int(_settings.get_setting("display", "resolution")) < 0:
		show_resolution(DisplayServer.window_get_size())
	_syncing = false

# A window whose size is not a preset selects nothing and is written onto the button, rather than
# left blank (reads as broken) or rounded to a neighbour (claims a size it is not).
func show_resolution(size: Vector2i) -> void:
	var options := control("display", "resolution") as OptionButton
	if options == null or _settings == null:
		return
	var index: int = _settings.resolution_index_of(size)
	options.select(options.get_item_index(index))
	if index < 0:
		options.text = "%d x %d" % [size.x, size.y]

func _build_tab(group: String, rows: Array) -> void:
	var scroll := ScrollContainer.new()
	scroll.name = group.capitalize()
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_tabs.add_child(scroll)
	var grid := GridContainer.new()
	grid.name = "Rows"
	grid.columns = 2
	grid.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	grid.add_theme_constant_override("h_separation", 18)
	grid.add_theme_constant_override("v_separation", 8)
	scroll.add_child(grid)
	for row in rows:
		var label := Label.new()
		label.text = row["label"]
		label.tooltip_text = row["hint"]
		# A Label ignores the mouse by default, and with it its own tooltip.
		label.mouse_filter = Control.MOUSE_FILTER_PASS
		grid.add_child(label)
		grid.add_child(_make_control(group, row))

func _make_control(group: String, row: Dictionary) -> Control:
	var name: String = row["name"]
	var key := group + "/" + name
	var c: Control
	match String(row["kind"]):
		"bool":
			var check := CheckBox.new()
			check.toggled.connect(_on_write.bind(group, name))
			c = check
		"int", "float":
			var slider := HSlider.new()
			slider.min_value = row["ui_min"]
			slider.max_value = row["ui_max"]
			slider.step = row["step"]
			slider.custom_minimum_size = Vector2(240, 0)
			slider.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			slider.value_changed.connect(_on_write.bind(group, name))
			slider.name = name
			slider.tooltip_text = row["hint"]
			_controls[key] = slider
			var value_label := Label.new()
			value_label.custom_minimum_size = Vector2(72, 0)
			_value_labels[key] = value_label
			var box := HBoxContainer.new()
			box.add_child(slider)
			box.add_child(value_label)
			return box
		"enum":
			var options := OptionButton.new()
			var labels: PackedStringArray = row["options"]
			for i in range(labels.size()):
				options.add_item(labels[i], i)
			options.item_selected.connect(_on_option.bind(options, group, name))
			c = options
		"color":
			var picker := ColorPickerButton.new()
			picker.edit_alpha = false
			picker.custom_minimum_size = Vector2(72, 24)
			picker.color_changed.connect(_on_write.bind(group, name))
			c = picker
	c.name = name
	c.tooltip_text = row["hint"]
	_controls[key] = c
	return c

func _on_write(value, group: String, name: String) -> void:
	if _syncing:
		return
	if group == SUN_RAYS:
		_sun_rays.set(name, value)
		_update_value_label(group, name)
		return
	if _settings == null:
		return
	_settings.set_setting(group, name, value)
	_update_value_label(group, name)
	# Deliberately no full resync mid-drag: rewriting a slider from the clamped value fights the
	# pointer. The tier is the one write that moves other rows, so it resyncs.
	if group == "render" and name == "quality_tier":
		sync_from_settings()

func _on_option(index: int, options: OptionButton, group: String, name: String) -> void:
	_on_write(options.get_item_id(index), group, name)

func _update_value_label(group: String, name: String) -> void:
	var label: Label = _value_labels.get(group + "/" + name)
	var value = _read(group, name)
	if label == null or value == null:
		return
	label.text = str(value) if typeof(value) == TYPE_INT else "%.3f" % float(value)

func _on_reset() -> void:
	if _settings:
		_settings.reset_to_shipped()
	if _sun_rays:
		for row in _sun_rays_rows():
			var name: String = row["name"]
			# enabled is CompositorEffect's own property, on by default; the rest are the script's.
			_sun_rays.set(name, true if name == "enabled" else _sun_rays.get_script().get_property_default_value(name))
	sync_from_settings()

func _on_close() -> void:
	set_open(false)
