extends Control
# Minecraft's Create New World screen, cut to its two world-shaping options: a seed and a world
# type (spec §6.3). World name, game mode and the rest are out of scope.

const TYPES_DIR := "res://demo/world_types"
const MAIN_SCENE := "res://demo/scenes/main.tscn"
const TITLE_SCENE := "res://demo/scenes/title.tscn"
const GroundSpawn := preload("res://demo/scripts/ground_spawn.gd")
# More digits than this and to_int() overflows int64. Minecraft hashes whatever
# Long.parseLong rejects, and so does this.
const MAX_SEED_DIGITS := 18

# The scene switch. A member so a test can catch the built world instead of replacing
# gdUnit's own scene.
var launch := func(world: Node) -> void: get_tree().change_scene_to_node(world)

var _types: Array[WorldType] = []
var _type_index := 0

# Blank -> random non-zero; an integer -> itself; anything else -> its hash. 32 bits either
# way, the width VoxelWorld.world_seed stores.
static func parse_seed(text: String) -> int:
	var t := text.strip_edges()
	if t.is_empty():
		return randi_range(1, 0x7fffffff)
	if t.is_valid_int() and t.trim_prefix("+").trim_prefix("-").length() <= MAX_SEED_DIGITS:
		return t.to_int() & 0xffffffff
	return t.hash() & 0xffffffff

# Every WorldType .tres in dir_path, by file name -- which is why the shipped files carry an
# order prefix (00_default, 10_mesas, 20_flat).
static func load_types(dir_path: String) -> Array[WorldType]:
	var names := PackedStringArray()
	for f in DirAccess.get_files_at(dir_path):
		# An exported build lists "<name>.tres.remap"; load() takes the original name.
		f = f.trim_suffix(".remap")
		if f.ends_with(".tres") and not names.has(f):
			names.append(f)
	names.sort()
	var out: Array[WorldType] = []
	for f in names:
		var r := load(dir_path.path_join(f))
		if r is WorldType:
			out.append(r)
		else:
			push_warning("world types: %s is not a WorldType; skipped" % dir_path.path_join(f))
	return out

# The demo scene with the chosen type and seed. Both are set BEFORE the instance enters the
# tree: the terrain pipeline loads once, at the world's first init.
static func build_world(type: WorldType, seed: int) -> Node:
	var main: Node = load(MAIN_SCENE).instantiate()
	var world: VoxelWorldScene = main.get_node("VoxelWorld")
	world.world_type = type
	world.world_seed = seed
	var spawn: Node = GroundSpawn.new()
	spawn.name = "GroundSpawn"
	main.add_child(spawn)
	return main

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_types = load_types(TYPES_DIR)
	(%WorldType as Button).pressed.connect(_next_type)
	(%Create as Button).pressed.connect(_create)
	(%Cancel as Button).pressed.connect(_cancel)
	(%Seed as LineEdit).text_submitted.connect(func(_text: String) -> void: _create())
	_show_type()
	(%Seed as LineEdit).grab_focus()

# _input, not _unhandled_input: the focused LineEdit would otherwise see Esc first.
func _input(event: InputEvent) -> void:
	if event.is_action_pressed("ui_cancel"):
		get_viewport().set_input_as_handled()
		_cancel()

func _show_type() -> void:
	var has := not _types.is_empty()
	(%WorldType as Button).disabled = not has
	(%Create as Button).disabled = not has
	(%WorldType as Button).text = "World Type: %s" % (
			_types[_type_index].display_name if has else "none found")

func _next_type() -> void:
	if _types.is_empty():
		return
	_type_index = (_type_index + 1) % _types.size()
	_show_type()

func _create() -> void:
	if _types.is_empty():
		return
	launch.call(build_world(_types[_type_index], parse_seed((%Seed as LineEdit).text)))

func _cancel() -> void:
	get_tree().change_scene_to_file(TITLE_SCENE)
