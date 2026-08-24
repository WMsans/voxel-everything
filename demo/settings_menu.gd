extends Control
# Graphics settings popup for the demo shell (F7).
#
# The frame is almost entirely screen-space, so what it costs is set with a handful of dials
# rather than with an algorithm. Until now the only way to move them was the benchmark's
# command line (demo/benchmark.gd's --near-scale / --render-scale / --upscaler / --quality),
# which is no use to someone running the demo. This panel is those same dials, live.
#
# It deliberately keeps no state of its own: F1's beauty menu, the benchmark's flags and this
# popup all write the same VoxelWorld and the same Viewport, so every open re-reads them
# (sync_from_world) instead of trusting a copy that could be stale.
#
# The one thing it refuses to do is override a measured run -- see is_measured_run().

const CONFIG_SECTION := "graphics"

# Offered window sizes, ascending. project.godot ships 2560x1440. The entry matching the
# current window is the one selected; a size that is not in the table selects nothing and is
# written onto the button verbatim (see show_resolution) rather than rounded to a neighbour.
const RESOLUTIONS: Array[Vector2i] = [
	Vector2i(1280, 720),
	Vector2i(1600, 900),
	Vector2i(1920, 1080),
	Vector2i(2560, 1440),
	Vector2i(3840, 2160),
]

# Label plus the Viewport.scaling_3d_mode it selects. Same set the benchmark's --upscaler=
# accepts, so a configuration found in the UI can be reproduced on the command line.
const UPSCALERS := [
	["Bilinear", Viewport.SCALING_3D_MODE_BILINEAR],
	["FSR 1", Viewport.SCALING_3D_MODE_FSR],
	["FSR 2", Viewport.SCALING_3D_MODE_FSR2],
	["MetalFX spatial", Viewport.SCALING_3D_MODE_METALFX_SPATIAL],
	["MetalFX temporal", Viewport.SCALING_3D_MODE_METALFX_TEMPORAL],
]

const QUALITY_TIERS := ["Off", "Low", "Medium", "High"]

@export var world_path: NodePath
# Empty means "the viewport this panel is drawn into", which is what the demo wants. A test
# points it at a SubViewport it owns so asserting on scaling_3d_scale cannot change how the
# rest of the suite renders.
@export var viewport_path: NodePath
@export var config_path := "user://graphics.cfg"
@export var toggle_key := KEY_F7

var _world: VoxelWorld
var _viewport: Viewport
# What the scene and project shipped, captured before any saved config is applied. Reset
# returns here rather than to numbers duplicated from main.tscn, which would drift from it.
var _shipped := {}
var _syncing := false
var _mouse_mode_before := Input.MOUSE_MODE_CAPTURED

@onready var _resolution: OptionButton = %Resolution
@onready var _fullscreen: CheckBox = %Fullscreen
@onready var _render_scale: HSlider = %RenderScale
@onready var _render_scale_value: Label = %RenderScaleValue
@onready var _upscaler: OptionButton = %Upscaler
@onready var _near_scale: HSlider = %NearScale
@onready var _near_scale_value: Label = %NearScaleValue
@onready var _near_field: CheckBox = %NearField
@onready var _quality: OptionButton = %Quality

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	_world = get_node_or_null(world_path) as VoxelWorld
	_viewport = get_viewport()
	if not viewport_path.is_empty():
		_viewport = get_node_or_null(viewport_path) as Viewport
	_populate()
	_connect()
	_shipped = _snapshot()
	var cfg := ConfigFile.new()
	if cfg.load(config_path) == OK:
		var args := OS.get_cmdline_user_args()
		apply_saved_config(cfg, args)
		apply_saved_window(cfg, args)
	sync_from_world()
	visible = false

func _unhandled_input(event: InputEvent) -> void:
	if not (event is InputEventKey) or not event.pressed or event.echo:
		return
	if event.keycode == toggle_key:
		set_open(not visible)
		get_viewport().set_input_as_handled()
	elif visible and event.keycode == KEY_ESCAPE:
		# Closing beats player.gd's Esc (release the mouse): while this panel is up, the
		# mouse is already free and Esc reads as "dismiss the dialog".
		set_open(false)
		get_viewport().set_input_as_handled()

# Opening frees the cursor so the panel can be clicked, and closing puts back whatever mode
# was in force -- the demo starts mouse-captured, but a player who released it with Esc
# before opening this should not find it recaptured on close.
func set_open(open: bool) -> void:
	if open == visible:
		return
	visible = open
	if open:
		_mouse_mode_before = Input.mouse_mode
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
		sync_from_world()
	else:
		Input.mouse_mode = _mouse_mode_before
		save_config()

func _exit_tree() -> void:
	# Quitting with the panel open still persists what was set in it.
	if visible:
		save_config()

# --- the measured-run guard -------------------------------------------------------------
# demo/benchmark.gd and demo/capture.gd set these same dials from their own flags, and
# docs/PORTFOLIO.md reports numbers against them. A settings file left behind by a play
# session that silently moved render scale would make every subsequent run unreadable, so a
# measured run ignores saved settings entirely.
func is_measured_run(args: PackedStringArray) -> bool:
	for arg in args:
		if arg.begins_with("--benchmark") or arg == "--capture":
			return true
	return false

# --- persistence ------------------------------------------------------------------------
# Split in two on purpose: the renderer dials are per-frame cost and safe to apply anywhere,
# while the window ones reach DisplayServer and change global display state.
func apply_saved_config(cfg: ConfigFile, args: PackedStringArray) -> bool:
	if is_measured_run(args):
		return false
	if _world:
		_world.near_field_scale = float(cfg.get_value(
			CONFIG_SECTION, "near_field_scale", _world.near_field_scale))
		_world.set_effect_enabled("near_field", bool(cfg.get_value(
			CONFIG_SECTION, "near_field", _world.get_effect_enabled("near_field"))))
		_world.set_quality_tier(int(cfg.get_value(
			CONFIG_SECTION, "quality_tier", _world.quality_tier)))
	if _viewport:
		_viewport.scaling_3d_scale = float(cfg.get_value(
			CONFIG_SECTION, "render_scale", _viewport.scaling_3d_scale))
		_viewport.scaling_3d_mode = int(cfg.get_value(
			CONFIG_SECTION, "upscaler", _viewport.scaling_3d_mode))
	sync_from_world()
	return true

func apply_saved_window(cfg: ConfigFile, args: PackedStringArray) -> bool:
	if is_measured_run(args):
		return false
	var full := bool(cfg.get_value(CONFIG_SECTION, "fullscreen", _is_fullscreen()))
	if full != _is_fullscreen():
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN if full \
			else DisplayServer.WINDOW_MODE_WINDOWED)
	var index := int(cfg.get_value(CONFIG_SECTION, "resolution", -1))
	# Not rounded to a neighbour and not applied when it already matches: booting should not
	# churn the window just because the saved size happens to be the size it already is.
	if index >= 0 and index < RESOLUTIONS.size() and not full \
			and RESOLUTIONS[index] != DisplayServer.window_get_size():
		DisplayServer.window_set_size(RESOLUTIONS[index])
	sync_from_world()
	return true

func save_config() -> void:
	var cfg := ConfigFile.new()
	cfg.load(config_path)   # keep any key this panel does not own
	if _world:
		cfg.set_value(CONFIG_SECTION, "near_field_scale", _world.near_field_scale)
		cfg.set_value(CONFIG_SECTION, "near_field", _world.get_effect_enabled("near_field"))
		cfg.set_value(CONFIG_SECTION, "quality_tier", _world.quality_tier)
	if _viewport:
		cfg.set_value(CONFIG_SECTION, "render_scale", _viewport.scaling_3d_scale)
		cfg.set_value(CONFIG_SECTION, "upscaler", _viewport.scaling_3d_mode)
	cfg.set_value(CONFIG_SECTION, "resolution",
		resolution_index_of(DisplayServer.window_get_size()))
	cfg.set_value(CONFIG_SECTION, "fullscreen", _is_fullscreen())
	cfg.save(config_path)

# --- table lookups ----------------------------------------------------------------------
func resolution_index_of(size: Vector2i) -> int:
	return RESOLUTIONS.find(size)

# Point the dropdown at a window size. A size that is not one of the presets selects nothing
# and is written onto the button instead: leaving it blank reads as a broken control, and
# adding a synthetic entry for it would shift the indices the saved config stores.
func show_resolution(size: Vector2i) -> void:
	var index := resolution_index_of(size)
	_resolution.select(index)
	if index < 0:
		_resolution.text = "%d x %d" % [size.x, size.y]

func upscaler_index_of(mode: int) -> int:
	for i in range(UPSCALERS.size()):
		if int(UPSCALERS[i][1]) == mode:
			return i
	return -1

# --- state ------------------------------------------------------------------------------
func sync_from_world() -> void:
	_syncing = true
	if _world:
		_near_scale.value = _world.near_field_scale
		_near_field.set_pressed_no_signal(_world.get_effect_enabled("near_field"))
		_quality.select(clampi(_world.quality_tier, 0, QUALITY_TIERS.size() - 1))
	if _viewport:
		_render_scale.value = _viewport.scaling_3d_scale
		_upscaler.select(upscaler_index_of(_viewport.scaling_3d_mode))
	show_resolution(DisplayServer.window_get_size())
	_fullscreen.set_pressed_no_signal(_is_fullscreen())
	_update_value_labels()
	_syncing = false

func _snapshot() -> Dictionary:
	var d := {}
	if _world:
		d["near_field_scale"] = _world.near_field_scale
		d["near_field"] = _world.get_effect_enabled("near_field")
		d["quality_tier"] = _world.quality_tier
	if _viewport:
		d["render_scale"] = _viewport.scaling_3d_scale
		d["upscaler"] = _viewport.scaling_3d_mode
	return d

func reset_to_shipped() -> void:
	if _world and _shipped.has("near_field_scale"):
		_world.near_field_scale = float(_shipped["near_field_scale"])
		_world.set_effect_enabled("near_field", bool(_shipped["near_field"]))
		_world.set_quality_tier(int(_shipped["quality_tier"]))
	if _viewport and _shipped.has("render_scale"):
		_viewport.scaling_3d_scale = float(_shipped["render_scale"])
		_viewport.scaling_3d_mode = int(_shipped["upscaler"])
	sync_from_world()

# --- handlers ---------------------------------------------------------------------------
# Each reads back from the thing it wrote rather than echoing its own argument, so the label
# shows the value that was actually taken: VoxelWorld clamps near_field_scale to [0.1, 1.0].
func _on_near_scale(value: float) -> void:
	if _syncing or _world == null:
		return
	_world.near_field_scale = value
	_update_value_labels()

func _on_render_scale(value: float) -> void:
	if _syncing or _viewport == null:
		return
	_viewport.scaling_3d_scale = value
	_update_value_labels()

func _on_upscaler(index: int) -> void:
	if _syncing or _viewport == null or index < 0 or index >= UPSCALERS.size():
		return
	_viewport.scaling_3d_mode = int(UPSCALERS[index][1])

func _on_near_field(on: bool) -> void:
	if _syncing or _world == null:
		return
	_world.set_effect_enabled("near_field", on)

func _on_quality(index: int) -> void:
	if _syncing or _world == null:
		return
	_world.set_quality_tier(index)

func _on_resolution(index: int) -> void:
	if _syncing or index < 0 or index >= RESOLUTIONS.size():
		return
	if _is_fullscreen():
		DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_WINDOWED)
	DisplayServer.window_set_size(RESOLUTIONS[index])
	sync_from_world()

func _on_fullscreen(on: bool) -> void:
	if _syncing:
		return
	DisplayServer.window_set_mode(DisplayServer.WINDOW_MODE_FULLSCREEN if on \
		else DisplayServer.WINDOW_MODE_WINDOWED)
	sync_from_world()

func _on_reset() -> void:
	reset_to_shipped()

func _on_close() -> void:
	set_open(false)

# --- wiring -----------------------------------------------------------------------------
func _populate() -> void:
	for size in RESOLUTIONS:
		_resolution.add_item("%d x %d" % [size.x, size.y])
	for entry in UPSCALERS:
		_upscaler.add_item(String(entry[0]))
	for label in QUALITY_TIERS:
		_quality.add_item(label)

func _connect() -> void:
	_near_scale.value_changed.connect(_on_near_scale)
	_render_scale.value_changed.connect(_on_render_scale)
	_upscaler.item_selected.connect(_on_upscaler)
	_near_field.toggled.connect(_on_near_field)
	_quality.item_selected.connect(_on_quality)
	_resolution.item_selected.connect(_on_resolution)
	_fullscreen.toggled.connect(_on_fullscreen)
	(%Reset as Button).pressed.connect(_on_reset)
	(%Close as Button).pressed.connect(_on_close)

func _update_value_labels() -> void:
	if _world:
		_near_scale_value.text = _percent(_world.near_field_scale)
	if _viewport:
		_render_scale_value.text = _percent(_viewport.scaling_3d_scale)

func _percent(v: float) -> String:
	return "%d%%" % roundi(v * 100.0)

func _is_fullscreen() -> bool:
	var mode := DisplayServer.window_get_mode()
	return mode == DisplayServer.WINDOW_MODE_FULLSCREEN \
		or mode == DisplayServer.WINDOW_MODE_EXCLUSIVE_FULLSCREEN
