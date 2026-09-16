extends GdUnitTestSuite
# VoxelSettings (docs/superpowers/specs/2026-09-16-settings-store-design.md §3.5). Every node here
# points at a SubViewport the test owns, a throwaway config path and manage_window = false, so
# nothing reaches the real window or a developer's user://settings.cfg.

const CONFIG_PATH := "user://test_voxel_settings.cfg"
const OTHER_PATH := "user://test_voxel_settings_other.cfg"

var _roots: Array = []

func remove_configs() -> void:
	for path in [CONFIG_PATH, OTHER_PATH]:
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))

func before_test() -> void:
	remove_configs()

func after_test() -> void:
	for root in _roots:
		if is_instance_valid(root):
			root.free()
	_roots.clear()
	remove_configs()

# [world, viewport, settings]. near_field_scale and the viewport scale are what demo/main.tscn and
# project.godot ship, so "shipped" has known values.
func make_settings(config_path := CONFIG_PATH) -> Array:
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
	settings.config_path = config_path
	settings.manage_window = false
	root.add_child(settings)
	return [world, vp, settings]

func test_render_beauty_and_grass_dials_set_before_ready_reach_world() -> void:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.world_path = NodePath("../World")
	settings.config_path = CONFIG_PATH
	settings.manage_window = false
	assert_bool(settings.set_setting("render", "near_field_scale", 0.8)).is_true()
	assert_bool(settings.set_setting("beauty", "ssgi_taps", 4)).is_true()
	assert_bool(settings.set_setting("grass", "reach_m", 30.0)).is_true()
	root.add_child(settings)
	assert_float(world.near_field_scale).is_equal_approx(0.8, 0.001)
	assert_float(world.get_effect_value("ssgi_taps")).is_equal_approx(4.0, 0.001)
	assert_float(world.get_grass_value("reach_m")).is_equal_approx(30.0, 0.001)

func test_groups_are_listed_in_panel_order() -> void:
	var settings: VoxelSettings = make_settings()[2]
	assert_array(Array(settings.groups())).is_equal(["display", "render", "beauty", "grass"])

func test_describe_lists_every_row_with_its_value() -> void:
	var settings: VoxelSettings = make_settings()[2]
	for group in settings.groups():
		var rows: Array = settings.describe(group)
		assert_array(rows).override_failure_message(group).is_not_empty()
		for row in rows:
			for key in ["name", "label", "hint", "kind", "min", "max", "ui_min", "ui_max", "step",
					"options", "value", "default"]:
				assert_bool(row.has(key)).override_failure_message("%s/%s lacks %s" % [group, row.get("name"), key]).is_true()
			assert_array(["bool", "int", "float", "color", "enum"]).contains([row["kind"]])
			assert_that(row["value"]).is_equal(settings.get_setting(group, row["name"]))
	var kinds := {}
	for row in settings.describe("beauty"):
		kinds[row["name"]] = row["kind"]
	assert_str(kinds["ssgi_taps"]).is_equal("int")
	assert_str(kinds["ambient"]).is_equal("color")
	var upscaler: Dictionary = settings.describe("display").filter(func(r): return r["name"] == "upscaler")[0]
	assert_int(upscaler["options"].size()).is_equal(5)

func test_render_beauty_and_grass_dials_reach_the_world() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var settings: VoxelSettings = parts[2]
	assert_bool(settings.set_setting("render", "near_field_scale", 0.8)).is_true()
	assert_float(world.near_field_scale).is_equal_approx(0.8, 0.001)
	assert_bool(settings.set_setting("beauty", "ssgi_taps", 4)).is_true()
	assert_float(world.get_effect_value("ssgi_taps")).is_equal_approx(4.0, 0.001)
	assert_bool(settings.set_setting("grass", "reach_m", 30.0)).is_true()
	assert_float(world.get_grass_value("reach_m")).is_equal_approx(30.0, 0.001)
	assert_bool(settings.set_setting("beauty", "no_such_knob", 1.0)).is_false()
	assert_bool(settings.set_setting("nowhere", "ssgi", true)).is_false()
	assert_bool(settings.set_setting("beauty", "ambient", 0.5)).is_false()

func test_display_dials_reach_the_viewport() -> void:
	var parts := make_settings()
	var vp: SubViewport = parts[1]
	var settings: VoxelSettings = parts[2]
	settings.set_setting("display", "render_scale", 0.9)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.9, 0.001)
	settings.set_setting("display", "upscaler", 2)
	assert_int(vp.scaling_3d_mode).is_equal(Viewport.SCALING_3D_MODE_FSR2)

func test_the_display_base_is_the_viewport_at_ready() -> void:
	var settings: VoxelSettings = make_settings()[2]
	assert_float(float(settings.get_setting("display", "render_scale"))).is_equal_approx(0.65, 0.001)
	assert_dict(settings.get_overrides("display")).is_empty()

func test_what_the_scene_set_is_an_override() -> void:
	var settings: VoxelSettings = make_settings()[2]
	var render: Dictionary = settings.get_overrides("render")
	assert_float(float(render["near_field_scale"])).is_equal_approx(0.4, 0.001)

func test_overrides_round_trip_through_save_and_a_fresh_node() -> void:
	var settings: VoxelSettings = make_settings()[2]
	settings.set_setting("beauty", "ssgi_strength", 2.0)
	settings.set_setting("render", "near_field_scale", 0.75)
	settings.set_setting("display", "render_scale", 0.85)
	assert_bool(settings.save()).is_true()

	var second := make_settings()
	var world2: VoxelWorld = second[0]
	var vp2: SubViewport = second[1]
	assert_float(world2.get_effect_value("ssgi_strength")).is_equal_approx(2.0, 0.001)
	assert_float(world2.near_field_scale).is_equal_approx(0.75, 0.001)
	assert_float(vp2.scaling_3d_scale).is_equal_approx(0.85, 0.001)

# demo/benchmark.gd and demo/capture.gd set these dials from their own flags, and PORTFOLIO reports
# against them. A saved file that moved a measured number, or a measured run that overwrote the
# player's file, would make every run unreadable.
func test_a_measured_run_neither_loads_nor_saves() -> void:
	var first: VoxelSettings = make_settings()[2]
	first.set_setting("render", "near_field_scale", 0.75)
	assert_bool(first.save()).is_true()

	var second := make_settings(OTHER_PATH)
	var world2: VoxelWorld = second[0]
	var settings2: VoxelSettings = second[2]
	settings2.config_path = CONFIG_PATH
	assert_bool(settings2.apply_config(PackedStringArray(["--benchmark"]))).is_false()
	assert_float(world2.near_field_scale).is_equal_approx(0.4, 0.001)
	world2.near_field_scale = 0.3
	assert_bool(settings2.save()).is_false()
	var cfg := ConfigFile.new()
	assert_int(cfg.load(CONFIG_PATH)).is_equal(OK)
	assert_float(float(cfg.get_value("render", "near_field_scale", 0.0))).is_equal_approx(0.75, 0.001)

	assert_bool(settings2.apply_config(PackedStringArray([]))).is_true()
	assert_float(world2.near_field_scale).is_equal_approx(0.75, 0.001)

func test_every_benchmark_leg_is_a_measured_run() -> void:
	for leg in ["--benchmark", "--benchmark-move", "--benchmark-ridge", "--benchmark-edit",
			"--benchmark-edit-bounded", "--benchmark-island", "--capture"]:
		assert_bool(VoxelSettings.is_measured_args(PackedStringArray([leg]))) \
			.override_failure_message("%s must not be overridden by saved settings" % leg).is_true()
	assert_bool(VoxelSettings.is_measured_args(PackedStringArray(["--something-else"]))).is_false()

func test_reset_restores_what_the_scene_shipped() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var vp: SubViewport = parts[1]
	var settings: VoxelSettings = parts[2]
	settings.set_setting("render", "near_field_scale", 1.0)
	settings.set_setting("render", "near_field", false)
	settings.set_setting("display", "render_scale", 1.0)
	settings.set_setting("beauty", "ssr", false)
	settings.reset_to_shipped()
	assert_float(world.near_field_scale).is_equal_approx(0.4, 0.001)
	assert_bool(world.get_effect_enabled("near_field")).is_true()
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.65, 0.001)
	assert_bool(world.get_effect_enabled("ssr")).is_true()
	assert_dict(settings.get_overrides("beauty")).is_empty()

func test_clearing_beauty_overrides_returns_to_the_tier() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var settings: VoxelSettings = parts[2]
	settings.set_setting("render", "quality_tier", 1)
	settings.set_setting("beauty", "ssgi_strength", 2.0)
	settings.clear_overrides("beauty")
	assert_float(world.get_effect_value("ssgi_strength")).is_equal_approx(1.0, 0.001)
	assert_float(world.get_effect_value("ssgi_taps")).is_equal_approx(0.0, 0.001)

# Cel-shaded objects read ve_ambient; VoxelWorld publishes it from the beauty snapshot each frame.
func test_an_ambient_change_reaches_the_object_global() -> void:
	var settings: VoxelSettings = make_settings()[2]
	assert_bool(settings.set_setting("beauty", "ambient", Color(0.5, 0.4, 0.3))).is_true()
	await get_tree().process_frame
	await get_tree().process_frame
	var published: Vector3 = RenderingServer.global_shader_parameter_get("ve_ambient")
	settings.clear_overrides("beauty")
	await get_tree().process_frame
	assert_vector(published).is_equal_approx(Vector3(0.5, 0.4, 0.3), Vector3(0.001, 0.001, 0.001))

func test_inspector_properties_mirror_every_row() -> void:
	var parts := make_settings()
	var world: VoxelWorld = parts[0]
	var settings: VoxelSettings = parts[2]
	var names := {}
	for p in settings.get_property_list():
		names[p["name"]] = p
	for group in settings.groups():
		for row in settings.describe(group):
			var path := "%s/%s" % [group, row["name"]]
			assert_bool(names.has(path)).override_failure_message("no property %s" % path).is_true()
	settings.set("beauty/ssgi_strength", 2.5)
	assert_float(world.get_effect_value("ssgi_strength")).is_equal_approx(2.5, 0.001)
	assert_float(float(settings.get("beauty/ssgi_strength"))).is_equal_approx(2.5, 0.001)
	assert_bool(settings.property_can_revert("beauty/ssgi_strength")).is_true()
	assert_float(float(settings.property_get_revert("beauty/ssgi_strength"))).is_equal_approx(1.0, 0.001)

func test_property_values_set_before_ready_apply_to_the_world() -> void:
	var root := Node.new()
	add_child(root)
	_roots.append(root)
	var world: VoxelWorld = ClassDB.instantiate("VoxelWorld")
	world.name = "World"
	world.use_local_device = true
	world.physics_enabled = false
	root.add_child(world)
	var vp := SubViewport.new()
	vp.name = "Viewport"
	vp.render_target_update_mode = SubViewport.UPDATE_DISABLED
	root.add_child(vp)
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.world_path = NodePath("../World")
	settings.viewport_path = NodePath("../Viewport")
	settings.config_path = CONFIG_PATH
	settings.manage_window = false
	# What loading a scene does: properties first, _ready later.
	settings.set("beauty/ssgi_strength", 3.0)
	settings.set("render/near_field_scale", 0.9)
	settings.set("display/render_scale", 0.75)
	root.add_child(settings)
	assert_float(world.get_effect_value("ssgi_strength")).is_equal_approx(3.0, 0.001)
	assert_float(world.near_field_scale).is_equal_approx(0.9, 0.001)
	assert_float(vp.scaling_3d_scale).is_equal_approx(0.75, 0.001)
	# ...and they are what the scene shipped.
	assert_float(float(settings.get_overrides("beauty")["ssgi_strength"])).is_equal_approx(3.0, 0.001)

func test_a_packed_scene_stores_only_real_overrides() -> void:
	var holder := Node.new()
	var settings: VoxelSettings = ClassDB.instantiate("VoxelSettings")
	settings.name = "Settings"
	holder.add_child(settings)
	settings.owner = holder
	settings.set("beauty/ssgi_strength", 2.0)
	var packed := PackedScene.new()
	assert_int(packed.pack(holder)).is_equal(OK)
	var state := packed.get_state()
	var stored: Array = []
	for i in range(state.get_node_property_count(1)):
		stored.append(String(state.get_node_property_name(1, i)))
	holder.free()
	assert_array(stored).contains(["beauty/ssgi_strength"])
	assert_array(stored).not_contains(["beauty/ssgi_taps", "render/quality_tier", "grass/reach_m",
		"display/render_scale"])
