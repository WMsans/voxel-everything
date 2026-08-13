extends Node
# THROWAWAY SPIKE 2: SDF sphere-tracing driven through the HW raytracing pipeline.

func _ready() -> void:
	var rd := RenderingServer.create_local_rendering_device()
	if rd == null:
		print("local device failed; falling back to global")
		rd = RenderingServer.get_rendering_device()
	print("=== SPIKE 2: SDF sphere-trace + HW raytracing ===")
	print("device: ", rd.get_device_name())
	print("SUPPORTS_RAYTRACING_PIPELINE: ", rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE))
	print("SUPPORTS_RAY_QUERY: ", rd.has_feature(RenderingDevice.SUPPORTS_RAY_QUERY))

	if not rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE):
		print("ABORT: no raytracing pipeline support")
		get_tree().quit()
		return

	var raygen := """#version 460
#extension GL_EXT_ray_tracing : require

layout(set = 0, binding = 0) uniform accelerationStructureEXT tlas;
layout(set = 0, binding = 1, rgba32f) uniform image2D resultImage;
layout(location = 0) rayPayloadEXT vec3 payload;

void main() {
	vec2 size = vec2(gl_LaunchSizeEXT.xy);
	vec2 uv = vec2(gl_LaunchIDEXT.xy) / size;
	vec3 origin = vec3(0.0, 0.0, -3.0);
	vec3 dir = normalize(vec3(uv * 2.0 - 1.0, 2.0));

	// HW-accelerated traversal: hit the bounding quad.
	payload = vec3(0.0);
	traceRayEXT(tlas, gl_RayFlagsOpaqueEXT, 0xFF, 0, 0, 0, origin, 0.001, dir, 100.0, 0);
	float boxHit = payload.x;

	// Analytic SDF sphere-trace (sphere at origin, radius 0.5).
	float t = 0.0;
	float hit = 0.0;
	for (int i = 0; i < 96; i++) {
		vec3 p = origin + dir * t;
		float d = length(p) - 0.5;
		if (d < 0.001) { hit = 1.0; break; }
		t += d;
		if (t > 100.0) break;
	}

	vec3 color = vec3(0.02, 0.02, 0.03);
	if (hit > 0.5) {
		vec3 p = origin + dir * t;
		vec3 n = normalize(p);
		float lambert = max(dot(n, normalize(vec3(0.5, 0.8, 0.3))), 0.0);
		color = vec3(0.2, 0.5, 1.0) * (0.15 + 0.85 * lambert);
	}
	if (boxHit > 0.5) {
		color += vec3(0.0, 0.1, 0.0);
	}
	imageStore(resultImage, ivec2(gl_LaunchIDEXT.xy), vec4(color, 1.0));
}
"""
	var miss := """#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT vec3 payload;
void main() { payload = vec3(0.0); }
"""
	var closest_hit := """#version 460
#extension GL_EXT_ray_tracing : require
layout(location = 0) rayPayloadInEXT vec3 payload;
void main() { payload = vec3(1.0, 0.0, 0.0); }
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

	var hit_sbt := rd.hit_sbt_create(pipeline, 1)
	var sbt_range := rd.hit_sbt_range_alloc(hit_sbt, 1)
	rd.hit_sbt_range_update(hit_sbt, sbt_range, 0, PackedInt32Array([0]))
	print("hit_sbt range=", sbt_range)

	# Bounding quad at z=-0.6 spanning [-0.5, 0.5]^2.
	var verts := PackedFloat32Array([
		-0.5, -0.5, -0.6,
		 0.5, -0.5, -0.6,
		 0.5,  0.5, -0.6,
		-0.5,  0.5, -0.6,
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
	rd.blas_build(blas)
	var tlas := rd.tlas_create(1, 0)
	var inst := RDAccelerationStructureInstance.new()
	inst.transform = Transform3D()
	inst.blas = blas
	inst.hit_sbt_range = sbt_range
	inst.flags = RenderingDevice.ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT
	inst.mask = 0xFF
	rd.tlas_build(tlas, [inst])

	var img_w := 32
	var img_h := 32
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

	var rlist := rd.raytracing_list_begin()
	rd.raytracing_list_bind_raytracing_pipeline(rlist, pipeline)
	rd.raytracing_list_bind_uniform_set(rlist, uset, 0)
	rd.raytracing_list_trace_rays(rlist, 0, hit_sbt, img_w, img_h, 1)
	rd.raytracing_list_end()

	rd.submit()
	rd.sync()

	var data := rd.texture_get_data(out_img, 0)
	print("readback bytes: ", data.size())
	if data.size() == 0:
		print("READBACK FAILED")
		get_tree().quit()
		return
	var floats := data.to_float32_array()

	# Print a coarse ASCII map of the result.
	var sphere_count := 0
	var box_count := 0
	for y in range(0, img_h, 2):
		var line := ""
		for x in img_w:
			var i := (y * img_w + x) * 4
			var r := floats[i]
			var g := floats[i + 1]
			var b := floats[i + 2]
			if b > 0.4 and r < 0.6:
				line += "S"  # sphere (blue)
				sphere_count += 1
			elif g > 0.09:
				line += "."  # box hit only (green tint)
				box_count += 1
			else:
				line += " "
		print(line)
	print("sphere pixels: ", sphere_count, "  box-only pixels: ", box_count)
	print("center px rgb: ", floats[(16 * img_w + 16) * 4], floats[(16 * img_w + 16) * 4 + 1], floats[(16 * img_w + 16) * 4 + 2])

	get_tree().quit()
