extends Node
# THROWAWAY SPIKE: prove hardware raytracing works through Godot's RenderingDevice.

func _ready() -> void:
	var rd := RenderingServer.get_rendering_device()
	print("=== SPIKE: HW Raytracing via RenderingDevice ===")
	print("device: ", rd.get_device_name())
	print("vendor: ", rd.get_device_vendor_name())
	print("SUPPORTS_RAYTRACING_PIPELINE: ", rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE))
	print("SUPPORTS_RAY_QUERY: ", rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY))
	print("SUPPORTS_BUFFER_DEVICE_ADDRESS: ", rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_DEVICE_ADDRESS))

	if not rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE):
		print("ABORT: no raytracing pipeline support")
		get_tree().quit()
		return

	# ---------------- Shaders ----------------
	var raygen := """#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform image2D resultImage;
layout(location = 0) rayPayloadEXT vec3 hitColor;

void main() {
	vec2 size = vec2(gl_LaunchSizeEXT.xy);
	vec2 uv = vec2(gl_LaunchIDEXT.xy) / size;
	vec3 origin = vec3(uv.x * 2.0 - 1.0, uv.y * 2.0 - 1.0, -5.0);
	vec3 dir = vec3(0.0, 0.0, 1.0);
	hitColor = vec3(0.05, 0.05, 0.1);
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, origin, 0.001, dir, 100.0, 0);
	imageStore(resultImage, ivec2(gl_LaunchIDEXT.xy), vec4(hitColor, 1.0));
}
"""
	var miss := """#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitColor;

void main() {
	hitColor = vec3(0.0, 0.0, 0.0);
}
"""
	var closest_hit := """#version 460
#extension GL_EXT_ray_tracing : require

layout(location = 0) rayPayloadInEXT vec3 hitColor;

void main() {
	hitColor = vec3(1.0, 0.0, 0.0);
}
"""

	var src := RDShaderSource.new()
	src.language = RenderingDevice.SHADER_LANGUAGE_GLSL
	src.source_raygen = raygen
	src.source_miss = miss
	src.source_closest_hit = closest_hit

	var spirv := rd.shader_compile_spirv_from_source(src)
	if spirv.compile_error_raygen != "" or spirv.compile_error_miss != "" or spirv.compile_error_closest_hit != "":
		print("SHADER COMPILE ERROR raygen:\n", spirv.compile_error_raygen)
		print("SHADER COMPILE ERROR miss:\n", spirv.compile_error_miss)
		print("SHADER COMPILE ERROR closest_hit:\n", spirv.compile_error_closest_hit)
		get_tree().quit()
		return
	print("shader compile OK")

	var shader_rid := rd.shader_create_from_spirv(spirv)
	print("shader rid valid: ", shader_rid.is_valid())

	# ---------------- Pipeline ----------------
	var raygen_ps := RDPipelineShader.new()
	raygen_ps.shader = shader_rid
	var miss_ps := RDPipelineShader.new()
	miss_ps.shader = shader_rid
	var chit_ps := RDPipelineShader.new()
	chit_ps.shader = shader_rid

	var hit_group := RDHitGroup.new()
	hit_group.closest_hit_shader = chit_ps

	var pipeline := rd.raytracing_pipeline_create([raygen_ps], [miss_ps], [hit_group], 1)
	print("pipeline valid: ", rd.raytracing_pipeline_is_valid(pipeline))

	# ---------------- Hit SBT ----------------
	var hit_sbt := rd.hit_sbt_create(pipeline, 1)
	var sbt_range := rd.hit_sbt_range_alloc(hit_sbt, 1)
	var upd := rd.hit_sbt_range_update(hit_sbt, sbt_range, 0, PackedInt32Array([0]))
	print("hit_sbt range=", sbt_range, " update_err=", upd)

	# ---------------- Triangle geometry (quad covering [-0.5,0.5]^2 at z=0) ----------------
	var verts := PackedFloat32Array([
		-0.5, -0.5, 0.0,
		 0.5, -0.5, 0.0,
		 0.5,  0.5, 0.0,
		-0.5,  0.5, 0.0,
	])
	var vb := rd.vertex_buffer_create(verts.size() * 4, verts.to_byte_array())
	var idx := PackedInt32Array([0, 1, 2, 0, 2, 3])
	var ib := rd.index_buffer_create(idx.size(), RenderingDevice.INDEX_BUFFER_FORMAT_UINT32, idx.to_byte_array())

	var geom := RDAccelerationStructureGeometry.new()
	geom.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
	geom.vertex_buffer = vb
	geom.vertex_stride = 12
	geom.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
	geom.vertex_count = 4
	geom.index_buffer = ib
	geom.index_count = 6

	var blas := rd.blas_create([geom], 0)
	print("blas valid: ", blas.is_valid())
	var b_err := rd.blas_build(blas)
	print("blas_build err: ", b_err)

	var tlas := rd.tlas_create(1, 0)
	var inst := RDAccelerationStructureInstance.new()
	inst.transform = Transform3D()
	inst.blas = blas
	inst.hit_sbt_range = sbt_range
	inst.flags = RenderingDevice.ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT
	inst.mask = 0xFF
	var t_err := rd.tlas_build(tlas, [inst])
	print("tlas_build err: ", t_err)

	rd.full_barrier()

	# ---------------- Output image ----------------
	var img_w := 16
	var img_h := 16
	var fmt := RDTextureFormat.new()
	fmt.format = RenderingDevice.DATA_FORMAT_R32G32B32A32_SFLOAT
	fmt.width = img_w
	fmt.height = img_h
	fmt.depth = 1
	fmt.array_layers = 1
	fmt.mipmaps = 1
	fmt.texture_type = RenderingDevice.TEXTURE_TYPE_2D
	fmt.usage_bits = RenderingDevice.TEXTURE_USAGE_STORAGE_BIT | RenderingDevice.TEXTURE_USAGE_CAN_COPY_FROM_BIT
	var out_img := rd.texture_create(fmt, RDTextureView.new(), [])

	# ---------------- Uniform set ----------------
	var u_tlas := RDUniform.new()
	u_tlas.uniform_type = RenderingDevice.UNIFORM_TYPE_ACCELERATION_STRUCTURE
	u_tlas.binding = 0
	u_tlas.add_id(tlas)

	var u_img := RDUniform.new()
	u_img.uniform_type = RenderingDevice.UNIFORM_TYPE_IMAGE
	u_img.binding = 1
	u_img.add_id(out_img)

	var uset := rd.uniform_set_create([u_tlas, u_img], shader_rid, 0)
	print("uniform_set valid: ", rd.uniform_set_is_valid(uset))

	# ---------------- Trace ----------------
	var rlist := rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(rlist, pipeline)
	rd.raytracing_list_bind_uniform_set(rlist, uset, 0)
	rd.raytracing_list_trace_rays(rlist, 0, hit_sbt, img_w, img_h, 1)
	rd.raytracing_list_end()

	rd.submit()
	rd.sync()

	# ---------------- Readback ----------------
	var data := rd.texture_get_data(out_img, 0)
	print("readback bytes: ", data.size())
	if data.size() == 0:
		print("READBACK FAILED")
		get_tree().quit()
		return

	var floats := data.to_float32_array()
	print("pixel count: ", floats.size() / 4)
	var idx_c := (8 * img_w + 8) * 4
	var idx_0 := 0
	print("corner(0,0) rgba: ", floats[idx_0], ",", floats[idx_0 + 1], ",", floats[idx_0 + 2], ",", floats[idx_0 + 3])
	print("center(8,8) rgba: ", floats[idx_c], ",", floats[idx_c + 1], ",", floats[idx_c + 2], ",", floats[idx_c + 3])

	var hits := 0
	for y in img_h:
		for x in img_w:
			var i := (y * img_w + x) * 4
			if floats[i] > 0.5:
				hits += 1
	print("hits (red px): ", hits, " / ", img_w * img_h)

	get_tree().quit()
