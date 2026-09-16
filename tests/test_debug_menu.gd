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

# The grass density slider reaches the grass store the same way a magnitude knob reaches
# ve::BeautySettings -- and it must move only its own field.
func test_grass_density_slider_writes_only_its_named_knob() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	var reach: float = world.get_grass_value("reach_m")
	var slider: HSlider = menu.get_node("Controls/blades_per_brick")
	slider.emit_signal("value_changed", 21.0)
	assert_float(world.get_grass_value("blades_per_brick")).is_equal_approx(21.0, 0.001)
	assert_float(world.get_grass_value("reach_m")).is_equal_approx(reach, 0.001)

# The grass checkbox toggles the enabled flag through set_grass_value, mirroring how an
# effect checkbox writes its named field.
func test_grass_checkbox_writes_the_enabled_flag() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu: PanelContainer = pair[1]
	assert_float(world.get_grass_value("enabled")).is_equal_approx(1.0, 0.001)
	var enabled: CheckBox = menu.get_node("Controls/enabled")
	enabled.emit_signal("toggled", false)
	assert_float(world.get_grass_value("enabled")).is_equal_approx(0.0, 0.001)
	assert_bool(world.get_effect_enabled("outlines")).is_true()

# S5: a slider whose range excludes the knob's shipped value clamps the knob to the slider the
# first time it is dragged (blade width ships at 0.08 against a 0.06 slider), and a slider wider
# than the C++ clamp offers values the store silently refuses. The clamp is discovered on a
# separate world so the world under test is only read.
func assert_slider_range(name: String, ui_lo: float, ui_hi: float, shipped: float,
		lo: float, hi: float) -> void:
	assert_float(shipped).override_failure_message(
		"%s: shipped %f outside the slider [%f, %f]" % [name, shipped, ui_lo, ui_hi]
		).is_between(ui_lo, ui_hi)
	assert_float(ui_lo).override_failure_message(
		"%s: slider min %f below the C++ clamp %f" % [name, ui_lo, lo]).is_greater_equal(lo)
	assert_float(ui_hi).override_failure_message(
		"%s: slider max %f above the C++ clamp %f" % [name, ui_hi, hi]).is_less_equal(hi)

func test_every_slider_range_contains_the_shipped_value_and_sits_inside_the_clamp() -> void:
	var pair: Array = make_pair()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var probe: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	probe.use_local_device = true
	probe.physics_enabled = false
	pair[1].get_parent().add_child(probe)
	for entry in MENU_SCRIPT.VALUES:
		probe.set_effect_value(entry[1], 1e9)
		var hi: float = probe.get_effect_value(entry[1])
		probe.set_effect_value(entry[1], -1e9)
		var lo: float = probe.get_effect_value(entry[1])
		assert_slider_range(entry[1], entry[2], entry[3], world.get_effect_value(entry[1]), lo, hi)
	for entry in MENU_SCRIPT.GRASS_VALUES:
		probe.set_grass_value(entry[1], 1e9)
		var hi: float = probe.get_grass_value(entry[1])
		probe.set_grass_value(entry[1], -1e9)
		var lo: float = probe.get_grass_value(entry[1])
		assert_slider_range(entry[1], entry[2], entry[3], world.get_grass_value(entry[1]), lo, hi)
