extends GdUnitTestSuite
# demo/settings_menu.tscn — the graphics popup (F7).
#
# Two things these tests are careful about, because getting them wrong would reach outside
# the test run: the menu is pointed at a SubViewport the test owns rather than the root
# viewport (so a render-scale assertion cannot change how the rest of the suite renders),
# and at a throwaway config path (so a developer's real user://graphics.cfg is neither read
# nor written). Nothing here calls DisplayServer; window resolution is covered through the
# table it decodes, not by resizing the window the tests are running in.

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

# [world, viewport, menu]. near_field_scale is set to the value demo/main.tscn ships so the
# "reset restores what the scene shipped" test has a known boot value to return to.
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
	var menu = MENU_SCENE.instantiate()
	menu.name = "Menu"
	menu.world_path = NodePath("../World")
	menu.viewport_path = NodePath("../Viewport")
	menu.config_path = CONFIG_PATH
	root.add_child(menu)
	return [world, vp, menu]

func test_near_field_slider_writes_the_world_dial() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu = pair[2]
	menu.get_node("%NearScale").emit_signal("value_changed", 0.8)
	assert_float(world.near_field_scale).is_equal_approx(0.8, 0.001)

func test_render_scale_slider_writes_the_viewport() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var vp: SubViewport = pair[1]
	var menu = pair[2]
	menu.get_node("%RenderScale").emit_signal("value_changed", 0.9)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.9, 0.001)

func test_upscaler_option_writes_the_viewport_mode() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var vp: SubViewport = pair[1]
	var menu = pair[2]
	var index: int = menu.upscaler_index_of(Viewport.SCALING_3D_MODE_FSR2)
	assert_int(index).is_greater_equal(0)
	menu.get_node("%Upscaler").emit_signal("item_selected", index)
	assert_int(vp.scaling_3d_mode).is_equal(Viewport.SCALING_3D_MODE_FSR2)

func test_near_field_toggle_is_a_real_render_effect() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu = pair[2]
	assert_bool(world.get_effect_enabled("near_field")).is_true()
	menu.get_node("%NearField").emit_signal("toggled", false)
	assert_bool(world.get_effect_enabled("near_field")).is_false()

func test_quality_option_writes_the_world_tier() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu = pair[2]
	menu.get_node("%Quality").emit_signal("item_selected", 1)
	assert_int(world.quality_tier).is_equal(1)

# The panel keeps no shadow copy of the dials: F1's menu, the benchmark's flags and this
# popup all write the same world, so it re-reads on every open instead of trusting itself.
func test_opening_resyncs_from_the_world() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var menu = pair[2]
	world.near_field_scale = 0.55
	world.set_quality_tier(2)
	menu.sync_from_world()
	assert_float(float(menu.get_node("%NearScale").value)).is_equal_approx(0.55, 0.001)
	assert_int(menu.get_node("%Quality").selected).is_equal(2)

func test_saved_config_round_trips_into_a_fresh_menu() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	menu.get_node("%NearScale").emit_signal("value_changed", 0.75)
	menu.get_node("%RenderScale").emit_signal("value_changed", 0.85)
	menu.save_config()

	var second: Array = make_menu()
	await get_tree().process_frame
	var world2: VoxelWorld = second[0]
	var vp2: SubViewport = second[1]
	assert_float(world2.near_field_scale).is_equal_approx(0.75, 0.001)
	assert_float(vp2.scaling_3d_scale).is_equal_approx(0.85, 0.001)

# The benchmark sets these same dials from its own flags and PORTFOLIO reports against them.
# A saved config that silently moved a measured number would make every run unreadable.
func test_a_benchmark_run_ignores_the_saved_config() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	menu.get_node("%NearScale").emit_signal("value_changed", 0.75)
	menu.save_config()

	var second: Array = make_menu()
	await get_tree().process_frame
	var world2: VoxelWorld = second[0]
	var menu2 = second[2]
	world2.near_field_scale = 0.4
	var cfg := ConfigFile.new()
	assert_int(cfg.load(CONFIG_PATH)).is_equal(OK)
	assert_bool(menu2.apply_saved_config(cfg, PackedStringArray(["--benchmark"]))).is_false()
	assert_float(world2.near_field_scale).is_equal_approx(0.4, 0.001)
	assert_bool(menu2.apply_saved_config(cfg, PackedStringArray([]))).is_true()
	assert_float(world2.near_field_scale).is_equal_approx(0.75, 0.001)

func test_every_benchmark_leg_is_recognised_as_a_benchmark_run() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	for leg in ["--benchmark", "--benchmark-move", "--benchmark-ridge", "--benchmark-edit",
			"--benchmark-edit-bounded", "--benchmark-island", "--capture"]:
		assert_bool(menu.is_measured_run(PackedStringArray([leg]))) \
			.override_failure_message("%s must not be overridden by saved settings" % leg) \
			.is_true()
	assert_bool(menu.is_measured_run(PackedStringArray(["--something-else"]))).is_false()

func test_reset_restores_what_the_scene_shipped() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var world: VoxelWorld = pair[0]
	var vp: SubViewport = pair[1]
	var menu = pair[2]
	menu.get_node("%NearScale").emit_signal("value_changed", 1.0)
	menu.get_node("%RenderScale").emit_signal("value_changed", 1.0)
	menu.get_node("%NearField").emit_signal("toggled", false)
	menu.get_node("%Reset").emit_signal("pressed")
	assert_float(world.near_field_scale).is_equal_approx(0.4, 0.001)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.65, 0.001)
	assert_bool(world.get_effect_enabled("near_field")).is_true()

func test_resolution_table_is_ordered_and_offers_the_project_default() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	var table: Array = menu.RESOLUTIONS
	assert_int(table.size()).is_greater(1)
	for i in range(1, table.size()):
		assert_int(table[i].x).is_greater(table[i - 1].x)
	var shipped := Vector2i(
		ProjectSettings.get_setting("display/window/size/viewport_width"),
		ProjectSettings.get_setting("display/window/size/viewport_height"))
	assert_int(menu.resolution_index_of(shipped)).is_greater_equal(0)

# Dropdown labels are what the player picks from; a table whose text disagreed with the
# Vector2i it decodes to would be silently wrong in the only place it is visible.
func test_resolution_labels_match_the_sizes_they_apply() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	var options: OptionButton = menu.get_node("%Resolution")
	assert_int(options.item_count).is_equal(menu.RESOLUTIONS.size())
	for i in range(options.item_count):
		var size: Vector2i = menu.RESOLUTIONS[i]
		assert_str(options.get_item_text(i)).contains("%d" % size.x)
		assert_str(options.get_item_text(i)).contains("%d" % size.y)

# A window whose size is not in the table used to leave the dropdown blank, which reads as a
# broken control rather than as "this size is not one of the presets". It shows the real size
# instead, without claiming to be one of the entries.
func test_an_off_table_window_size_is_shown_rather_than_left_blank() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	var options: OptionButton = menu.get_node("%Resolution")

	menu.show_resolution(Vector2i(1920, 1080))
	assert_int(options.selected).is_equal(menu.resolution_index_of(Vector2i(1920, 1080)))

	menu.show_resolution(Vector2i(1337, 999))
	assert_int(options.selected).is_equal(-1)
	assert_str(options.text).contains("1337")
	assert_str(options.text).contains("999")

func key_event(code: int) -> InputEventKey:
	var ev := InputEventKey.new()
	ev.keycode = code
	ev.pressed = true
	return ev

# The binding itself, not just the handlers behind it. F7 toggles, Esc dismisses -- and Esc
# only while the panel is up, so it keeps meaning "release the mouse" to player.gd otherwise.
func test_f7_toggles_the_panel_and_escape_dismisses_it() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	assert_bool(menu.visible).is_false()

	menu._unhandled_input(key_event(KEY_F7))
	assert_bool(menu.visible).is_true()
	menu._unhandled_input(key_event(KEY_F7))
	assert_bool(menu.visible).is_false()

	menu._unhandled_input(key_event(KEY_ESCAPE))
	assert_bool(menu.visible).is_false()
	menu._unhandled_input(key_event(KEY_F7))
	menu._unhandled_input(key_event(KEY_ESCAPE))
	assert_bool(menu.visible).is_false()

# Closing persists, so a dial set in the panel survives a restart without an explicit save.
func test_closing_the_panel_persists_the_dials() -> void:
	var pair: Array = make_menu()
	await get_tree().process_frame
	var menu = pair[2]
	menu.set_open(true)
	menu.get_node("%NearScale").emit_signal("value_changed", 0.7)
	menu.set_open(false)

	var cfg := ConfigFile.new()
	assert_int(cfg.load(CONFIG_PATH)).is_equal(OK)
	assert_float(float(cfg.get_value("graphics", "near_field_scale", 0.0))) \
		.is_equal_approx(0.7, 0.001)
