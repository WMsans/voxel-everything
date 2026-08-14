extends GdUnitTestSuite

func test_local_rendering_device_compute_readback() -> void:
	var rd := RenderingServer.create_local_rendering_device()
	assert_object(rd).is_not_null()
	if rd == null:
		return

	var shader_src := """
#version 460
layout(local_size_x = 1, local_size_y = 1, local_size_z = 1) in;
layout(set = 0, binding = 0, std430) buffer Data { float v[]; } data;
void main() { data.v[0] = 42.0; }
"""
	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_compute = shader_src
	var spirv := rd.shader_compile_spirv_from_source(src)
	assert_str(spirv.compile_error_compute).is_empty()
	var shader := rd.shader_create_from_spirv(spirv)
	assert_bool(shader.is_valid()).is_true()

	var buffer := rd.storage_buffer_create(4, PackedFloat32Array([0.0]).to_byte_array())
	var uniform := RDUniform.new()
	uniform.uniform_type = RenderingDevice.UNIFORM_TYPE_STORAGE_BUFFER
	uniform.binding = 0
	uniform.add_id(buffer)
	var uset := rd.uniform_set_create([uniform], shader, 0)

	var pipeline := rd.compute_pipeline_create(shader)
	var list := rd.compute_list_begin()
	rd.compute_list_bind_compute_pipeline(list, pipeline)
	rd.compute_list_bind_uniform_set(list, uset, 0)
	rd.compute_list_dispatch(list, 1, 1, 1)
	rd.compute_list_end()
	rd.submit()
	rd.sync()

	var out := rd.buffer_get_data(buffer).to_float32_array()
	assert_float(out[0]).is_equal_approx(42.0, 0.001)

	rd.free_rid(buffer)
	rd.free_rid(shader)
	rd.free()
