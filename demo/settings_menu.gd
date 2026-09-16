extends Control
# The settings panel (F1): window, render budget, beauty and grass dials in one place.
#
# It keeps no tables and no state of its own. Every tab is built from VoxelSettings.describe(), so a
# new row in a C++ settings table appears here with its label, range and hint, and a slider range
# cannot drift from the clamp behind it (S5). Every open re-reads the values, because the benchmark's
# flags, the dev keys and the inspector write the same stores. Persistence and the measured-run guard
# live in VoxelSettings; closing the panel asks it to save.

@export var settings_path: NodePath
@export var toggle_key := KEY_F1

var _settings: VoxelSettings
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
			_build_tab(group)
	(%Reset as Button).pressed.connect(_on_reset)
	(%Close as Button).pressed.connect(_on_close)
	sync_from_settings()
	visible = false

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.keycode == toggle_key:
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
		if _settings:
			_settings.save()

func _exit_tree() -> void:
	# Quitting with the panel open still persists what was set in it.
	if visible and _settings:
		_settings.save()

# The control for one row, or null.
func control(group: String, name: String) -> Control:
	return _controls.get(group + "/" + name)

func sync_from_settings() -> void:
	if _settings == null:
		return
	_syncing = true
	for key in _controls:
		var parts: PackedStringArray = String(key).split("/")
		var value = _settings.get_setting(parts[0], parts[1])
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

func _build_tab(group: String) -> void:
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
	for row in _settings.describe(group):
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
	if _syncing or _settings == null:
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
	if label == null or _settings == null:
		return
	var value = _settings.get_setting(group, name)
	label.text = str(value) if typeof(value) == TYPE_INT else "%.3f" % float(value)

func _on_reset() -> void:
	if _settings:
		_settings.reset_to_shipped()
	sync_from_settings()

func _on_close() -> void:
	set_open(false)
