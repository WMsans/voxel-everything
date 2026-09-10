extends GdUnitTestSuite

const MENU_SCRIPT := preload("res://demo/debug_menu.gd")
var _roots: Array = []

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()

func make_pair() -> Array:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var menu := PanelContainer.new()
	menu.name = "Menu"
	menu.set_script(MENU_SCRIPT)
	menu.set("world_path", NodePath("../World"))
	root.add_child(menu)
	return [world, menu]

func test_quality_selection_replaces_the_world_settings() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	var quality: OptionButton = menu.get_node("Controls/Quality")
	quality.emit_signal("item_selected", 0)
	assert_int(world.quality_tier).is_equal(0)
	assert_bool(world.hooks().debug_beauty_settings()["ssr"]).is_false()
	assert_bool(world.hooks().debug_beauty_settings()["outlines"]).is_false()

func test_checkbox_writes_only_its_named_field() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	var outlines: CheckBox = menu.get_node("Controls/outlines")
	outlines.emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("outlines")).is_false()
	assert_bool(world.get_effect_enabled("ssr")).is_true()

func test_the_ssao_checkbox_writes_the_world_setting() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	assert_bool(world.get_effect_enabled("ssao")).is_true()
	var ssao: CheckBox = menu.get_node("Controls/ssao")
	ssao.emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("ssao")).is_false()
	assert_bool(world.get_effect_enabled("outlines")).is_true()

func test_islands_toggle_is_a_real_render_effect() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	assert_bool(world.get_effect_enabled("islands")).is_true()
	var islands: CheckBox = menu.get_node("Controls/islands")
	islands.emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("islands")).is_false()
	assert_bool(world.hooks().debug_beauty_settings()["islands"]).is_false()

# The magnitude knobs are the ones the emissive look is tuned with, so the slider has to
# reach ve::BeautySettings the same way a checkbox does -- and it must move only its own
# field, which is the failure a shared setter would produce.
func test_a_value_slider_writes_only_its_named_knob() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	var reach: float = world.get_effect_value("emissive_gi_radius")
	var slider: HSlider = menu.get_node("Controls/emissive_gi_strength")
	slider.emit_signal("value_changed", 21.0)
	assert_float(world.get_effect_value("emissive_gi_strength")).is_equal_approx(21.0, 0.001)
	assert_float(world.get_effect_value("emissive_gi_radius")).is_equal_approx(reach, 0.001)

# Out-of-range values are clamped by ve::clamp_settings, not stored raw: a negative strength
# would otherwise subtract light from the scene.
func test_knob_values_are_clamped_on_the_way_in() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	world.set_effect_value("emissive_gi_strength", -5.0)
	assert_float(world.get_effect_value("emissive_gi_strength")).is_equal_approx(0.0, 0.001)
	world.set_effect_value("ssgi_temporal", 1.0)
	assert_float(world.get_effect_value("ssgi_temporal")).is_less(1.0)
