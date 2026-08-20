extends GdUnitTestSuite

const BENCHMARK_SCRIPT := preload("res://demo/benchmark.gd")
var _nodes: Array = []

func after_test() -> void:
	for node in _nodes:
		if is_instance_valid(node):
			node.free()
	_nodes.clear()

func benchmark_node() -> Node:
	var node := Node.new()
	node.set_script(BENCHMARK_SCRIPT)
	add_child(node)
	_nodes.append(node)
	return node

func test_timing_condition_uses_verdict_qualified_key() -> void:
	var bench := benchmark_node()
	bench.set("_vsync_actual", "disabled")
	var line: String = str(bench.call("_timing_condition_line"))
	assert_str(line).contains("BENCH timing_condition")
	assert_str(line).contains("verdict_qualified=false")
	assert_str(line).not_contains("frame_verdict_qualified=")

func test_timing_condition_qualifies_wayland_vsync_fallback() -> void:
	var bench := benchmark_node()
	bench.set("_vsync_actual", "enabled")
	var line: String = str(bench.call("_timing_condition_line"))
	assert_str(line).contains("vsync_actual=enabled")
	assert_str(line).contains("verdict_qualified=true")

func test_wayland_disabled_readback_is_reported_as_enabled_fallback() -> void:
	var bench := benchmark_node()
	var actual: String = str(bench.call("_vsync_actual_from_readback", "Wayland",
			DisplayServer.VSYNC_DISABLED))
	assert_str(actual).is_equal("enabled")

func test_x11_disabled_readback_is_reported_as_disabled() -> void:
	var bench := benchmark_node()
	var actual: String = str(bench.call("_vsync_actual_from_readback", "X11",
			DisplayServer.VSYNC_DISABLED))
	assert_str(actual).is_equal("disabled")

func test_effects_off_args_parse_multiple_effect_names() -> void:
	var bench := benchmark_node()
	var names: PackedStringArray = bench.call("_effects_off_from_args",
		PackedStringArray(["--benchmark", "--effects-off=raymarched_sun_shadow,islands"]))
	assert_int(names.size()).is_equal(2)
	assert_str(names[0]).is_equal("raymarched_sun_shadow")
	assert_str(names[1]).is_equal("islands")

func test_effects_off_args_accumulate_repeated_options() -> void:
	var bench := benchmark_node()
	var names: PackedStringArray = bench.call("_effects_off_from_args",
		PackedStringArray(["--effects-off=raymarched_sun_shadow", "--effects-off=islands"]))
	assert_int(names.size()).is_equal(2)
	assert_str(names[0]).is_equal("raymarched_sun_shadow")
	assert_str(names[1]).is_equal("islands")
