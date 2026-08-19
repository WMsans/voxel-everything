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

func test_effects_off_args_parse_multiple_effect_names() -> void:
	var bench := benchmark_node()
	var names: PackedStringArray = bench.call("_effects_off_from_args",
		PackedStringArray(["--benchmark", "--effects-off=raymarched_sun_shadow,islands"]))
	assert_int(names.size()).is_equal(2)
	assert_str(names[0]).is_equal("raymarched_sun_shadow")
	assert_str(names[1]).is_equal("islands")
