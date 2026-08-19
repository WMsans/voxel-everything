extends GdUnitTestSuite
var _worlds:Array=[]
func after_test()->void:
	for w in _worlds:
		if is_instance_valid(w):w.free()
	_worlds.clear()
func world()->VoxelWorld:
	var w:VoxelWorld=ClassDB.instantiate("VoxelWorld");w.use_local_device=true
	add_child(w);_worlds.append(w);return w
func test_pairs_by_identity_and_sums_occurrences()->void:
	var d: Dictionary = world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:7:lod:1:e","engine","ve:7:frame:0:b","ve:7:lod:0:b","ve:7:lod:1:b",
		"ve:7:raymarch:0:e","ve:7:frame:0:e","ve:7:raymarch:0:b","ve:7:lod:0:e"]),
		PackedInt64Array([5600,77,1000,2000,5000,1900,9000,1100,3500]),42)
	assert_bool(d["valid"]).is_true()
	assert_float(d["raymarch_gpu_ms"]).is_equal_approx(.8,.0001)
	assert_float(d["lod_gpu_ms"]).is_equal_approx(2.1,.0001)
	assert_float(d["custom_frame_gpu_ms"]).is_equal_approx(8.0,.0001)
func test_bad_pairs_are_missing_not_zero()->void:
	var d: Dictionary = world().debug_ingest_gpu_timings(PackedStringArray([
		"ve:8:frame:0:b","ve:8:frame:0:e","ve:8:ssr:0:b","ve:8:lod:0:b",
		"ve:8:lod:0:e"]),PackedInt64Array([100,900,200,700,650]),43)
	assert_float(d["ssr_gpu_ms"]).is_equal(-1.0)
	assert_float(d["lod_gpu_ms"]).is_equal(-1.0)
	assert_int(d["dropped_pairs"]).is_equal(2)
func test_new_rd_frame_gets_new_sample_id()->void:
	var w: VoxelWorld = world();var n:=PackedStringArray(["ve:9:frame:0:b","ve:9:frame:0:e"])
	var a: Dictionary = w.debug_ingest_gpu_timings(n,PackedInt64Array([100,200]),50)
	var b: Dictionary = w.debug_ingest_gpu_timings(n,PackedInt64Array([300,500]),51)
	assert_int(b["sample_id"]).is_greater(int(a["sample_id"]))
func test_synthetic_ingest_declares_microseconds_without_live_scaling()->void:
	var d: Dictionary = world().debug_ingest_gpu_timings(
		PackedStringArray(["ve:10:frame:0:b","ve:10:frame:0:e"]),
		PackedInt64Array([6000000,6016000]),52)
	assert_bool(d["valid"]).is_true()
	assert_float(d["custom_frame_gpu_ms"]).is_equal_approx(16.0,.0001)
	assert_str(d["timestamp_unit"]).is_equal("synthetic_microseconds")
	assert_str(d["timestamp_normalization"]).is_equal("none_synthetic_inputs_are_microseconds")
	assert_float(d["timestamp_scale_to_microseconds"]).is_equal_approx(1.0,.0001)
