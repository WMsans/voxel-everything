extends GdUnitTestSuite
# demo/settings_menu.tscn — the unified settings panel (F1), built from VoxelSettings.describe().
#
# The panel is pointed at a VoxelSettings node that is pointed to a SubViewport the test owns (so a
# render-scale assertion cannot change how the rest of the suite renders), a throwaway config path
# (so a developer's user://settings.cfg is neither read nor written) and manage_window = false (so
# nothing resizes the window the tests run in). Ported cases keep their original names.

const MENU_SCENE := preload("res://demo/settings_menu.tscn")
const CONFIG_PATH := "user://test_settings_menu.cfg"

var _roots: Array = []

func before_test() -> void:
	DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()
	DirAccess.remove_absolute(ProjectSettings.globalize_path(CONFIG_PATH))
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

# [world, viewport, settings, menu]. near_field_scale and the viewport scale are what main.tscn and
# project.godot ship, so "reset restores what the scene shipped" has known values.
func make_menu() -> Array:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	world.near_field_scale = 0.4
	root.add_child(world)
	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	vp.scaling_3d_scale = 0.65
	root.add_child(vp)
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.name = "Settings"
	settings.world_path = NodePath("../World")
	settings.viewport_path = NodePath("../Viewport")
	settings.config_path = CONFIG_PATH
	settings.manage_window = false
	root.add_child(settings)
	var menu = MENU_SCENE.instantiate()
	menu.name = "Menu"
	menu.settings_path = NodePath("../Settings")
	root.add_child(menu)
	return [world, vp, settings, menu]

func key_event(code: int) -> InputEventKey:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.pressed = true
	return ev

# The binding itself. F1 toggles, Esc dismisses -- and Esc only while the panel is up, so it keeps
# meaning "release the mouse" to player.gd otherwise.
func test_f1_toggles_the_panel_and_escape_dismisses_it() -> void:
	var menu = make_menu()[3]
	await get_tree().process_frame
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F1))
	assert_bool(menu.visible).is_true()
	menu._unhandled_input(key_event(KEY_F1))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_ESCAPE))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F1))
	menu._unhandled_input(key_event(KEY_ESCAPE))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F7))
	assert_bool(menu.visible).is_false()

func test_every_group_has_a_tab_with_a_control_per_row() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var menu = parts[3]
	for group in settings.groups():
		for row in settings.describe(group):
			assert_object(menu.control(group, row["name"])).override_failure_message(
				"no control for %s/%s" % [group, row["name"]]).is_not_null()

# S5, ported: a slider never offers a value outside the clamp, and never excludes the value the
# dial holds. Ranges come from the rows, so this holds for every knob added later too.
func test_every_slider_range_contains_the_shipped_value_and_sits_inside_the_clamp() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var menu = parts[3]
	for group in settings.groups():
		for row in settings.describe(group):
			var slider := menu.control(group, row["name"]) as HSlider
			if slider == null:
				continue
			var name := "%s/%s" % [group, row["name"]]
			assert_float(slider.min_value).override_failure_message(name).is_greater_equal(float(row["min"]))
			assert_float(slider.max_value).override_failure_message(name).is_less_equal(float(row["max"]))
			assert_float(float(row["value"])).override_failure_message(
				"%s: %s outside the slider" % [name, row["value"]]).is_between(slider.min_value, slider.max_value)

func test_near_field_slider_writes_the_world_dial() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	parts[3].control("render", "near_field_scale").emit_signal("value_changed", 0.8)
	assert_float(parts[0].near_field_scale).is_equal_approx(0.8, 0.001)

func test_render_scale_slider_writes_the_viewport() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	parts[3].control("display", "render_scale").emit_signal("value_changed", 0.9)
	assert_float(parts[1].scaling_3d_scale).is_equal_approx(0.9, 0.001)

func test_upscaler_option_writes_the_viewport_mode() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var options := parts[3].control("display", "upscaler") as OptionButton
	options.emit_signal("item_selected", options.get_item_index(2)) # "FSR 2"
	assert_int(parts[1].scaling_3d_mode).is_equal(Viewport.SCALING_3D_MODE_FSR2)

func test_near_field_toggle_is_a_real_render_effect() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("near_field")).is_true()
	parts[3].control("render", "near_field").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("near_field")).is_false()

func test_islands_toggle_is_a_real_render_effect() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("islands")).is_true()
	parts[3].control("render", "islands").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("islands")).is_false()
	assert_bool(world.hooks().debug_beauty_settings()["islands"]).is_false()

# S6 through the panel: choosing a tier moves the untweaked knobs and keeps the tweaked one.
func test_quality_selection_replaces_the_world_settings() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var menu = parts[3]
	menu.control("beauty", "ssr").emit_signal("toggled", false)
	var quality := menu.control("render", "quality_tier") as OptionButton
	quality.emit_signal("item_selected", quality.get_item_index(0))
	assert_int(world.quality_tier).is_equal(0)
	assert_bool(world.hooks().debug_beauty_settings()["outlines"]).is_false()
	assert_bool(world.get_effect_enabled("ssr")).is_false()
	quality.emit_signal("item_selected", quality.get_item_index(3))
	assert_bool(world.get_effect_enabled("outlines")).is_true()
	assert_bool(world.get_effect_enabled("ssr")).is_false()

func test_checkbox_writes_only_its_named_field() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	parts[3].control("beauty", "outlines").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("outlines")).is_false()
	assert_bool(world.get_effect_enabled("ssr")).is_true()

func test_the_ssao_checkbox_writes_the_world_setting() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_bool(world.get_effect_enabled("ssao")).is_true()
	parts[3].control("beauty", "ssao").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("ssao")).is_false()
	assert_bool(world.get_effect_enabled("outlines")).is_true()

func test_a_value_slider_writes_only_its_named_knob() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var reach: float = world.get_effect_value("emissive_gi_radius")
	parts[3].control("beauty", "emissive_gi_strength").emit_signal("value_changed", 21.0)
	assert_float(world.get_effect_value("emissive_gi_strength")).is_equal_approx(21.0, 0.001)
	assert_float(world.get_effect_value("emissive_gi_radius")).is_equal_approx(reach, 0.001)

func test_knob_values_are_clamped_on_the_way_in() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	world.set_effect_value("emissive_gi_strength", -5.0)
	assert_float(world.get_effect_value("emissive_gi_strength")).is_equal_approx(0.0, 0.001)
	world.set_effect_value("ssgi_temporal", 1.0)
	assert_float(world.get_effect_value("ssgi_temporal")).is_less(1.0)

func test_grass_density_slider_writes_only_its_named_knob() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var reach: float = world.get_grass_value("reach_m")
	parts[3].control("grass", "blades_per_brick").emit_signal("value_changed", 21.0)
	assert_float(world.get_grass_value("blades_per_brick")).is_equal_approx(21.0, 0.001)
	assert_float(world.get_grass_value("reach_m")).is_equal_approx(reach, 0.001)

func test_grass_checkbox_writes_the_enabled_flag() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	assert_float(world.get_grass_value("enabled")).is_equal_approx(1.0, 0.001)
	parts[3].control("grass", "enabled").emit_signal("toggled", false)
	assert_float(world.get_grass_value("enabled")).is_equal_approx(0.0, 0.001)
	assert_bool(world.get_effect_enabled("outlines")).is_true()

func test_the_ambient_picker_writes_the_beauty_colour() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	parts[3].control("beauty", "ambient").emit_signal("color_changed", Color(0.3, 0.2, 0.1))
	var ambient: Color = parts[2].get_setting("beauty", "ambient")
	assert_float(ambient.r).is_equal_approx(0.3, 0.001)

# The panel keeps no shadow copy: the benchmark's flags, dev keys and the inspector write the same
# stores, so it re-reads on every open instead of trusting itself.
func test_opening_resyncs_from_the_settings() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var menu = parts[3]
	world.near_field_scale = 0.55
	world.set_quality_tier(2)
	menu.set_open(true)
	assert_float(float((menu.control("render", "near_field_scale") as HSlider).value)).is_equal_approx(0.55, 0.001)
	var quality := menu.control("render", "quality_tier") as OptionButton
	assert_int(quality.get_selected_id()).is_equal(2)
	menu.set_open(false)

func test_closing_the_panel_persists_the_dials() -> void:
	var menu = make_menu()[3]
	await get_tree().process_frame
	menu.set_open(true)
	menu.control("render", "near_field_scale").emit_signal("value_changed", 0.7)
	menu.set_open(false)
	var cfg := ConfigFile.new()
	assert_int(cfg.load(CONFIG_PATH)).is_equal(OK)
	assert_float(float(cfg.get_value("render", "near_field_scale", 0.0))).is_equal_approx(0.7, 0.001)

func test_reset_restores_what_the_scene_shipped() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = parts[0]
	var vp: SubViewport = parts[1]
	var menu = parts[3]
	menu.control("render", "near_field_scale").emit_signal("value_changed", 1.0)
	menu.control("display", "render_scale").emit_signal("value_changed", 1.0)
	menu.control("render", "near_field").emit_signal("toggled", false)
	menu.get_node("%Reset").emit_signal("pressed")
	assert_float(world.near_field_scale).is_equal_approx(0.4, 0.001)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.65, 0.001)
	assert_bool(world.get_effect_enabled("near_field")).is_true()

func test_the_resolution_options_offer_the_project_default() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var shipped := Vector2i(
		ProjectSettings.get_setting("display/window/size/viewport_width"),
		ProjectSettings.get_setting("display/window/size/viewport_height"))
	assert_int(settings.resolution_index_of(shipped)).is_greater_equal(0)

# A window whose size is not a preset used to leave the dropdown blank, which reads as a broken
# control. It shows the real size instead, without claiming to be one of the entries.
func test_an_off_table_window_size_is_shown_rather_than_left_blank() -> void:
	var parts := make_menu()
	await get_tree().process_frame
	var settings: VoxelSettings = parts[2]
	var menu = parts[3]
	var options := menu.control("display", "resolution") as OptionButton
	menu.show_resolution(Vector2i(1920, 1080))
	assert_int(options.get_selected_id()).is_equal(settings.resolution_index_of(Vector2i(1920, 1080)))
	menu.show_resolution(Vector2i(1337, 999))
	assert_int(options.selected).is_equal(-1)
	assert_str(options.text).contains("1337")
	assert_str(options.text).contains("999")
