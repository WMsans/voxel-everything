<!--
RenderingDevice API reference, converted from the official Godot documentation.
Source: https://github.com/godotengine/godot-docs/blob/master/classes/class_renderingdevice.rst
Original: https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html
-->

# RenderingDevice

**Inherits:** [Object](https://docs.godotengine.org/en/stable/classes/class_object.html)

Abstraction for working with modern low-level graphics APIs.

## Description

**RenderingDevice** is an abstraction for working with modern low-level graphics APIs such as Vulkan. Compared to [RenderingServer](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html) (which works with Godot's own rendering subsystems), **RenderingDevice** is much lower-level and allows working more directly with the underlying graphics APIs. **RenderingDevice** is used in Godot to provide support for several modern low-level graphics APIs while reducing the amount of code duplication required. **RenderingDevice** can also be used in your own projects to perform things that are not exposed by [RenderingServer](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html) or high-level nodes, such as using compute shaders.

On startup, Godot creates a global **RenderingDevice** which can be retrieved using [RenderingServer.get_rendering_device()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_get_rendering_device). This global **RenderingDevice** performs drawing to the screen.

**Local RenderingDevices:** Using [RenderingServer.create_local_rendering_device()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_create_local_rendering_device), you can create "secondary" rendering devices to perform drawing and GPU compute operations on separate threads.

**Note:** **RenderingDevice** assumes intermediate knowledge of modern graphics APIs such as Vulkan, Direct3D 12, Metal or WebGPU. These graphics APIs are lower-level than OpenGL or Direct3D 11, requiring you to perform what was previously done by the graphics driver itself. If you have difficulty understanding the concepts used in this class, follow the [Vulkan Tutorial](https://vulkan-tutorial.com/) or [Vulkan Guide](https://vkguide.dev/). It's recommended to have existing modern OpenGL or Direct3D 11 knowledge before attempting to learn a low-level graphics API.

**Note:** **RenderingDevice** is not available when running in headless mode or when using the Compatibility rendering method.

## Tutorials

- [Using compute shaders](https://docs.godotengine.org/en/stable/tutorials/shaders/compute_shaders.html)

## Methods

| Return | Method |
| --- | --- |
| void | [`barrier`](#barrier)`(from: bitfield[BarrierMask] = 32767, to: bitfield[BarrierMask] = 32767)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`blas_build`](#blas_build)`(blas: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`blas_create`](#blas_create)`(geometries: Array[RDAccelerationStructureGeometry], flags: bitfield[AccelerationStructureFlagBits])` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`buffer_clear`](#buffer_clear)`(buffer: RID, offset: int, size_bytes: int)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`buffer_copy`](#buffer_copy)`(src_buffer: RID, dst_buffer: RID, src_offset: int, dst_offset: int, size: int)` |
| [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) | [`buffer_get_data`](#buffer_get_data)`(buffer: RID, offset_bytes: int = 0, size_bytes: int = 0)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`buffer_get_data_async`](#buffer_get_data_async)`(buffer: RID, callback: Callable, offset_bytes: int = 0, size_bytes: int = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`buffer_get_device_address`](#buffer_get_device_address)`(buffer: RID)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`buffer_update`](#buffer_update)`(buffer: RID, offset: int, size_bytes: int, data: PackedByteArray)` |
| void | [`capture_timestamp`](#capture_timestamp)`(name: String)` |
| void | [`compute_list_add_barrier`](#compute_list_add_barrier)`(compute_list: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`compute_list_begin`](#compute_list_begin)`()` |
| void | [`compute_list_bind_compute_pipeline`](#compute_list_bind_compute_pipeline)`(compute_list: int, compute_pipeline: RID)` |
| void | [`compute_list_bind_uniform_set`](#compute_list_bind_uniform_set)`(compute_list: int, uniform_set: RID, set_index: int)` |
| void | [`compute_list_dispatch`](#compute_list_dispatch)`(compute_list: int, x_groups: int, y_groups: int, z_groups: int)` |
| void | [`compute_list_dispatch_indirect`](#compute_list_dispatch_indirect)`(compute_list: int, buffer: RID, offset: int)` |
| void | [`compute_list_end`](#compute_list_end)`()` |
| void | [`compute_list_set_push_constant`](#compute_list_set_push_constant)`(compute_list: int, buffer: PackedByteArray, size_bytes: int)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`compute_pipeline_create`](#compute_pipeline_create)`(shader: RID, specialization_constants: Array[RDPipelineSpecializationConstant] = [])` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`compute_pipeline_is_valid`](#compute_pipeline_is_valid)`(compute_pipeline: RID)` |
| [RenderingDevice](#renderingdevice) | [`create_local_device`](#create_local_device)`()` |
| void | [`draw_command_begin_label`](#draw_command_begin_label)`(name: String, color: Color)` |
| void | [`draw_command_end_label`](#draw_command_end_label)`()` |
| void | [`draw_command_insert_label`](#draw_command_insert_label)`(name: String, color: Color)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`draw_list_begin`](#draw_list_begin)`(framebuffer: RID, draw_flags: bitfield[DrawFlags] = 0, clear_color_values: PackedColorArray = PackedColorArray(), clear_depth_value: float = 1.0, clear_stencil_value: int = 0, region: Rect2 = Rect2(0, 0, 0, 0), breadcrumb: int = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`draw_list_begin_for_screen`](#draw_list_begin_for_screen)`(screen: int = 0, clear_color: Color = Color(0, 0, 0, 1))` |
| [PackedInt64Array](https://docs.godotengine.org/en/stable/classes/class_packedint64array.html) | [`draw_list_begin_split`](#draw_list_begin_split)`(framebuffer: RID, splits: int, initial_color_action: InitialAction, final_color_action: FinalAction, initial_depth_action: InitialAction, final_depth_action: FinalAction, clear_color_values: PackedColorArray = PackedColorArray(), clear_depth: float = 1.0, clear_stencil: int = 0, region: Rect2 = Rect2(0, 0, 0, 0), storage_textures: Array[RID] = [])` |
| void | [`draw_list_bind_index_array`](#draw_list_bind_index_array)`(draw_list: int, index_array: RID)` |
| void | [`draw_list_bind_render_pipeline`](#draw_list_bind_render_pipeline)`(draw_list: int, render_pipeline: RID)` |
| void | [`draw_list_bind_uniform_set`](#draw_list_bind_uniform_set)`(draw_list: int, uniform_set: RID, set_index: int)` |
| void | [`draw_list_bind_vertex_array`](#draw_list_bind_vertex_array)`(draw_list: int, vertex_array: RID)` |
| void | [`draw_list_bind_vertex_buffers_format`](#draw_list_bind_vertex_buffers_format)`(draw_list: int, vertex_format: int, vertex_count: int, vertex_buffers: Array[RID], offsets: PackedInt64Array = PackedInt64Array())` |
| void | [`draw_list_disable_scissor`](#draw_list_disable_scissor)`(draw_list: int)` |
| void | [`draw_list_draw`](#draw_list_draw)`(draw_list: int, use_indices: bool, instances: int, procedural_vertex_count: int = 0)` |
| void | [`draw_list_draw_indirect`](#draw_list_draw_indirect)`(draw_list: int, use_indices: bool, buffer: RID, offset: int = 0, draw_count: int = 1, stride: int = 0)` |
| void | [`draw_list_enable_scissor`](#draw_list_enable_scissor)`(draw_list: int, rect: Rect2 = Rect2(0, 0, 0, 0))` |
| void | [`draw_list_end`](#draw_list_end)`()` |
| void | [`draw_list_set_blend_constants`](#draw_list_set_blend_constants)`(draw_list: int, color: Color)` |
| void | [`draw_list_set_push_constant`](#draw_list_set_push_constant)`(draw_list: int, buffer: PackedByteArray, size_bytes: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`draw_list_switch_to_next_pass`](#draw_list_switch_to_next_pass)`()` |
| [PackedInt64Array](https://docs.godotengine.org/en/stable/classes/class_packedint64array.html) | [`draw_list_switch_to_next_pass_split`](#draw_list_switch_to_next_pass_split)`(splits: int)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`framebuffer_create`](#framebuffer_create)`(textures: Array[RID], validate_with_format: int = -1, view_count: int = 1)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`framebuffer_create_empty`](#framebuffer_create_empty)`(size: Vector2i, samples: TextureSamples = 0, validate_with_format: int = -1)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`framebuffer_create_multipass`](#framebuffer_create_multipass)`(textures: Array[RID], passes: Array[RDFramebufferPass], validate_with_format: int = -1, view_count: int = 1)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`framebuffer_format_create`](#framebuffer_format_create)`(attachments: Array[RDAttachmentFormat], view_count: int = 1)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`framebuffer_format_create_empty`](#framebuffer_format_create_empty)`(samples: TextureSamples = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`framebuffer_format_create_multipass`](#framebuffer_format_create_multipass)`(attachments: Array[RDAttachmentFormat], passes: Array[RDFramebufferPass], view_count: int = 1)` |
| [TextureSamples](#enum-texturesamples) | [`framebuffer_format_get_texture_samples`](#framebuffer_format_get_texture_samples)`(format: int, render_pass: int = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`framebuffer_get_format`](#framebuffer_get_format)`(framebuffer: RID)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`framebuffer_is_valid`](#framebuffer_is_valid)`(framebuffer: RID)` |
| void | [`free_rid`](#free_rid)`(rid: RID)` |
| void | [`full_barrier`](#full_barrier)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_captured_timestamp_cpu_time`](#get_captured_timestamp_cpu_time)`(index: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_captured_timestamp_gpu_time`](#get_captured_timestamp_gpu_time)`(index: int)` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_captured_timestamp_name`](#get_captured_timestamp_name)`(index: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_captured_timestamps_count`](#get_captured_timestamps_count)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_captured_timestamps_frame`](#get_captured_timestamps_frame)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_device_allocation_count`](#get_device_allocation_count)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_device_allocs_by_object_type`](#get_device_allocs_by_object_type)`(type: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_device_memory_by_object_type`](#get_device_memory_by_object_type)`(type: int)` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_device_name`](#get_device_name)`()` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_device_pipeline_cache_uuid`](#get_device_pipeline_cache_uuid)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_device_total_memory`](#get_device_total_memory)`()` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_device_vendor_name`](#get_device_vendor_name)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_driver_allocation_count`](#get_driver_allocation_count)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_driver_allocs_by_object_type`](#get_driver_allocs_by_object_type)`(type: int)` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_driver_and_device_memory_report`](#get_driver_and_device_memory_report)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_driver_memory_by_object_type`](#get_driver_memory_by_object_type)`(type: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_driver_resource`](#get_driver_resource)`(resource: DriverResource, rid: RID, index: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_driver_total_memory`](#get_driver_total_memory)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_frame_delay`](#get_frame_delay)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_memory_usage`](#get_memory_usage)`(type: MemoryType)` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_perf_report`](#get_perf_report)`()` |
| [String](https://docs.godotengine.org/en/stable/classes/class_string.html) | [`get_tracked_object_name`](#get_tracked_object_name)`(type_index: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`get_tracked_object_type_count`](#get_tracked_object_type_count)`()` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`has_feature`](#has_feature)`(feature: Features)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`hit_sbt_create`](#hit_sbt_create)`(raytracing_pipeline: RID, initial_hit_group_capacity: int)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`hit_sbt_range_alloc`](#hit_sbt_range_alloc)`(hit_sbt: RID, hit_group_count: int)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`hit_sbt_range_free`](#hit_sbt_range_free)`(hit_sbt: RID, range: int)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`hit_sbt_range_update`](#hit_sbt_range_update)`(hit_sbt: RID, range: int, offset: int, hit_group_indices: PackedInt32Array)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`hit_sbt_set_pipeline`](#hit_sbt_set_pipeline)`(hit_sbt: RID, raytracing_pipeline: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`index_array_create`](#index_array_create)`(index_buffer: RID, index_offset: int, index_count: int)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`index_buffer_create`](#index_buffer_create)`(size_indices: int, format: IndexBufferFormat, data: PackedByteArray = PackedByteArray(), use_restart_indices: bool = false, creation_bits: bitfield[BufferCreationBits] = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`limit_get`](#limit_get)`(limit: Limit)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`raytracing_list_begin`](#raytracing_list_begin)`()` |
| void | [`raytracing_list_bind_raytracing_pipeline`](#raytracing_list_bind_raytracing_pipeline)`(raytracing_list: int, raytracing_pipeline: RID)` |
| void | [`raytracing_list_bind_uniform_set`](#raytracing_list_bind_uniform_set)`(raytracing_list: int, uniform_set: RID, set_index: int)` |
| void | [`raytracing_list_end`](#raytracing_list_end)`()` |
| void | [`raytracing_list_set_push_constant`](#raytracing_list_set_push_constant)`(raytracing_list: int, buffer: PackedByteArray, size_bytes: int)` |
| void | [`raytracing_list_trace_rays`](#raytracing_list_trace_rays)`(raytracing_list: int, raygen_shader_index: int, hit_sbt: RID, width: int, height: int, depth: int)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`raytracing_pipeline_create`](#raytracing_pipeline_create)`(raygen_shaders: Array[RDPipelineShader], miss_shaders: Array[RDPipelineShader], hit_groups: Array[RDHitGroup], max_trace_recursion_depth: int)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`raytracing_pipeline_is_valid`](#raytracing_pipeline_is_valid)`(raytracing_pipeline: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`render_pipeline_create`](#render_pipeline_create)`(shader: RID, framebuffer_format: int, vertex_format: int, primitive: RenderPrimitive, rasterization_state: RDPipelineRasterizationState, multisample_state: RDPipelineMultisampleState, stencil_state: RDPipelineDepthStencilState, color_blend_state: RDPipelineColorBlendState, dynamic_state_flags: bitfield[PipelineDynamicStateFlags] = 0, for_render_pass: int = 0, specialization_constants: Array[RDPipelineSpecializationConstant] = [])` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`render_pipeline_is_valid`](#render_pipeline_is_valid)`(render_pipeline: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`sampler_create`](#sampler_create)`(state: RDSamplerState)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`sampler_is_format_supported_for_filter`](#sampler_is_format_supported_for_filter)`(format: DataFormat, sampler_filter: SamplerFilter)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`screen_get_framebuffer_format`](#screen_get_framebuffer_format)`(screen: int = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`screen_get_height`](#screen_get_height)`(screen: int = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`screen_get_width`](#screen_get_width)`(screen: int = 0)` |
| void | [`set_resource_name`](#set_resource_name)`(id: RID, name: String)` |
| [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) | [`shader_compile_binary_from_spirv`](#shader_compile_binary_from_spirv)`(spirv_data: RDShaderSPIRV, name: String = "")` |
| [RDShaderSPIRV](https://docs.godotengine.org/en/stable/classes/class_rdshaderspirv.html) | [`shader_compile_spirv_from_source`](#shader_compile_spirv_from_source)`(shader_source: RDShaderSource, allow_cache: bool = true)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`shader_create_from_bytecode`](#shader_create_from_bytecode)`(binary_data: PackedByteArray, placeholder_rid: RID = RID())` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`shader_create_from_spirv`](#shader_create_from_spirv)`(spirv_data: RDShaderSPIRV, name: String = "")` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`shader_create_placeholder`](#shader_create_placeholder)`()` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`shader_get_vertex_input_attribute_mask`](#shader_get_vertex_input_attribute_mask)`(shader: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`storage_buffer_create`](#storage_buffer_create)`(size_bytes: int, data: PackedByteArray = PackedByteArray(), usage: bitfield[StorageBufferUsage] = 0, creation_bits: bitfield[BufferCreationBits] = 0)` |
| void | [`submit`](#submit)`()` |
| void | [`sync`](#sync)`()` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`texture_buffer_create`](#texture_buffer_create)`(size_bytes: int, format: DataFormat, data: PackedByteArray = PackedByteArray())` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`texture_clear`](#texture_clear)`(texture: RID, color: Color, base_mipmap: int, mipmap_count: int, base_layer: int, layer_count: int)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`texture_copy`](#texture_copy)`(from_texture: RID, to_texture: RID, from_pos: Vector3, to_pos: Vector3, size: Vector3, src_mipmap: int, dst_mipmap: int, src_layer: int, dst_layer: int)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`texture_create`](#texture_create)`(format: RDTextureFormat, view: RDTextureView, data: Array[PackedByteArray] = [])` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`texture_create_from_extension`](#texture_create_from_extension)`(type: TextureType, format: DataFormat, samples: TextureSamples, usage_flags: bitfield[TextureUsageBits], image: int, width: int, height: int, depth: int, layers: int, mipmaps: int = 1)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`texture_create_shared`](#texture_create_shared)`(view: RDTextureView, with_texture: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`texture_create_shared_from_slice`](#texture_create_shared_from_slice)`(view: RDTextureView, with_texture: RID, layer: int, mipmap: int, mipmaps: int = 1, slice_type: TextureSliceType = 0)` |
| [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) | [`texture_get_data`](#texture_get_data)`(texture: RID, layer: int)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`texture_get_data_async`](#texture_get_data_async)`(texture: RID, layer: int, callback: Callable)` |
| [RDTextureFormat](https://docs.godotengine.org/en/stable/classes/class_rdtextureformat.html) | [`texture_get_format`](#texture_get_format)`(texture: RID)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`texture_get_native_handle`](#texture_get_native_handle)`(texture: RID)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`texture_is_discardable`](#texture_is_discardable)`(texture: RID)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`texture_is_format_supported_for_usage`](#texture_is_format_supported_for_usage)`(format: DataFormat, usage_flags: bitfield[TextureUsageBits])` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`texture_is_shared`](#texture_is_shared)`(texture: RID)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`texture_is_valid`](#texture_is_valid)`(texture: RID)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`texture_resolve_multisample`](#texture_resolve_multisample)`(from_texture: RID, to_texture: RID)` |
| void | [`texture_set_discardable`](#texture_set_discardable)`(texture: RID, discardable: bool)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`texture_update`](#texture_update)`(texture: RID, layer: int, data: PackedByteArray)` |
| [Error](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#enum_@globalscope_error) | [`tlas_build`](#tlas_build)`(tlas: RID, instances: Array[RDAccelerationStructureInstance])` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`tlas_create`](#tlas_create)`(max_instance_count: int, flags: bitfield[AccelerationStructureFlagBits])` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`uniform_buffer_create`](#uniform_buffer_create)`(size_bytes: int, data: PackedByteArray = PackedByteArray(), creation_bits: bitfield[BufferCreationBits] = 0)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`uniform_set_create`](#uniform_set_create)`(uniforms: Array[RDUniform], shader: RID, shader_set: int)` |
| [bool](https://docs.godotengine.org/en/stable/classes/class_bool.html) | [`uniform_set_is_valid`](#uniform_set_is_valid)`(uniform_set: RID)` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`vertex_array_create`](#vertex_array_create)`(vertex_count: int, vertex_format: int, src_buffers: Array[RID], offsets: PackedInt64Array = PackedInt64Array())` |
| [RID](https://docs.godotengine.org/en/stable/classes/class_rid.html) | [`vertex_buffer_create`](#vertex_buffer_create)`(size_bytes: int, data: PackedByteArray = PackedByteArray(), creation_bits: bitfield[BufferCreationBits] = 0)` |
| [int](https://docs.godotengine.org/en/stable/classes/class_int.html) | [`vertex_format_create`](#vertex_format_create)`(vertex_descriptions: Array[RDVertexAttribute])` |

## Enumerations

### enum DeviceType

- **DEVICE_TYPE_OTHER** = `0`

  Rendering device type does not match any of the other enum values or is unknown.

- **DEVICE_TYPE_INTEGRATED_GPU** = `1`

  Rendering device is an integrated GPU, which is typically *(but not always)* slower than dedicated GPUs ([DEVICE_TYPE_DISCRETE_GPU](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_device_type_discrete_gpu)). On Android and iOS, the rendering device type is always considered to be [DEVICE_TYPE_INTEGRATED_GPU](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_device_type_integrated_gpu).

- **DEVICE_TYPE_DISCRETE_GPU** = `2`

  Rendering device is a dedicated GPU, which is typically *(but not always)* faster than integrated GPUs ([DEVICE_TYPE_INTEGRATED_GPU](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_device_type_integrated_gpu)).

- **DEVICE_TYPE_VIRTUAL_GPU** = `3`

  Rendering device is an emulated GPU in a virtual environment. This is typically much slower than the host GPU, which means the expected performance level on a dedicated GPU will be roughly equivalent to [DEVICE_TYPE_INTEGRATED_GPU](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_device_type_integrated_gpu). Virtual machine GPU passthrough (such as VFIO) will not report the device type as [DEVICE_TYPE_VIRTUAL_GPU](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_device_type_virtual_gpu). Instead, the host GPU's device type will be reported as if the GPU was not emulated.

- **DEVICE_TYPE_CPU** = `4`

  Rendering device is provided by software emulation (such as Lavapipe or [SwiftShader](https://github.com/google/swiftshader)). This is the slowest kind of rendering device available; it's typically much slower than [DEVICE_TYPE_INTEGRATED_GPU](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_device_type_integrated_gpu).

- **DEVICE_TYPE_MAX** = `5`

  Represents the size of the [DeviceType](#enum-devicetype) enum.

### enum DriverResource

- **DRIVER_RESOURCE_LOGICAL_DEVICE** = `0`

  Specific device object based on a physical device (`rid` parameter is ignored).
  - Vulkan: Vulkan device driver resource (`VkDevice`).
  - D3D12: D3D12 device driver resource (`ID3D12Device`).
  - Metal: Metal device driver resource (`MTLDevice`).

- **DRIVER_RESOURCE_PHYSICAL_DEVICE** = `1`

  Physical device the specific logical device is based on (`rid` parameter is ignored).
  - Vulkan: `VkPhysicalDevice`.
  - D3D12: `IDXGIAdapter`.

- **DRIVER_RESOURCE_TOPMOST_OBJECT** = `2`

  Top-most graphics API entry object (`rid` parameter is ignored).
  - Vulkan: `VkInstance`.

- **DRIVER_RESOURCE_COMMAND_QUEUE** = `3`

  The main graphics-compute command queue (`rid` parameter is ignored).
  - Vulkan: `VkQueue`.
  - D3D12: `ID3D12CommandQueue`.
  - Metal: `MTLCommandQueue`.

- **DRIVER_RESOURCE_QUEUE_FAMILY** = `4`

  The specific family the main queue belongs to (`rid` parameter is ignored).
  - Vulkan: The queue family index, a `uint32_t`.

- **DRIVER_RESOURCE_TEXTURE** = `5`

  - Vulkan: `VkImage`.
  - D3D12: `ID3D12Resource`.

- **DRIVER_RESOURCE_TEXTURE_VIEW** = `6`

  The view of an owned or shared texture.
  - Vulkan: `VkImageView`.
  - D3D12: `ID3D12Resource`.

- **DRIVER_RESOURCE_TEXTURE_DATA_FORMAT** = `7`

  The native id of the data format of the texture.
  - Vulkan: `VkFormat`.
  - D3D12: `DXGI_FORMAT`.

- **DRIVER_RESOURCE_SAMPLER** = `8`

  - Vulkan: `VkSampler`.

- **DRIVER_RESOURCE_UNIFORM_SET** = `9`

  - Vulkan: `VkDescriptorSet`.

- **DRIVER_RESOURCE_BUFFER** = `10`

  Buffer of any kind of (storage, vertex, etc.).
  - Vulkan: `VkBuffer`.
  - D3D12: `ID3D12Resource`.

- **DRIVER_RESOURCE_COMPUTE_PIPELINE** = `11`

  - Vulkan: `VkPipeline`.
  - Metal: `MTLComputePipelineState`.

- **DRIVER_RESOURCE_RENDER_PIPELINE** = `12`

  - Vulkan: `VkPipeline`.
  - Metal: `MTLRenderPipelineState`.

- **DRIVER_RESOURCE_VULKAN_DEVICE** = `0`

  **Deprecated:** Use [DRIVER_RESOURCE_LOGICAL_DEVICE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_logical_device) instead.

- **DRIVER_RESOURCE_VULKAN_PHYSICAL_DEVICE** = `1`

  **Deprecated:** Use [DRIVER_RESOURCE_PHYSICAL_DEVICE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_physical_device) instead.

- **DRIVER_RESOURCE_VULKAN_INSTANCE** = `2`

  **Deprecated:** Use [DRIVER_RESOURCE_TOPMOST_OBJECT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_topmost_object) instead.

- **DRIVER_RESOURCE_VULKAN_QUEUE** = `3`

  **Deprecated:** Use [DRIVER_RESOURCE_COMMAND_QUEUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_command_queue) instead.

- **DRIVER_RESOURCE_VULKAN_QUEUE_FAMILY_INDEX** = `4`

  **Deprecated:** Use [DRIVER_RESOURCE_QUEUE_FAMILY](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_queue_family) instead.

- **DRIVER_RESOURCE_VULKAN_IMAGE** = `5`

  **Deprecated:** Use [DRIVER_RESOURCE_TEXTURE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_texture) instead.

- **DRIVER_RESOURCE_VULKAN_IMAGE_VIEW** = `6`

  **Deprecated:** Use [DRIVER_RESOURCE_TEXTURE_VIEW](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_texture_view) instead.

- **DRIVER_RESOURCE_VULKAN_IMAGE_NATIVE_TEXTURE_FORMAT** = `7`

  **Deprecated:** Use [DRIVER_RESOURCE_TEXTURE_DATA_FORMAT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_texture_data_format) instead.

- **DRIVER_RESOURCE_VULKAN_SAMPLER** = `8`

  **Deprecated:** Use [DRIVER_RESOURCE_SAMPLER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_sampler) instead.

- **DRIVER_RESOURCE_VULKAN_DESCRIPTOR_SET** = `9`

  **Deprecated:** Use [DRIVER_RESOURCE_UNIFORM_SET](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_uniform_set) instead.

- **DRIVER_RESOURCE_VULKAN_BUFFER** = `10`

  **Deprecated:** Use [DRIVER_RESOURCE_BUFFER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_buffer) instead.

- **DRIVER_RESOURCE_VULKAN_COMPUTE_PIPELINE** = `11`

  **Deprecated:** Use [DRIVER_RESOURCE_COMPUTE_PIPELINE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_compute_pipeline) instead.

- **DRIVER_RESOURCE_VULKAN_RENDER_PIPELINE** = `12`

  **Deprecated:** Use [DRIVER_RESOURCE_RENDER_PIPELINE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_render_pipeline) instead.

### enum DataFormat

- **DATA_FORMAT_R4G4_UNORM_PACK8** = `0`

  4-bit-per-channel red/green channel data format, packed into 8 bits. Values are in the `[0.0, 1.0]` range.

  **Note:** More information on all data formats can be found on the [Identification of formats](https://registry.khronos.org/vulkan/specs/1.1/html/vkspec.html#_identification_of_formats) section of the Vulkan specification, as well as the [VkFormat](https://registry.khronos.org/vulkan/specs/1.3-extensions/man/html/VkFormat.html) enum.

- **DATA_FORMAT_R4G4B4A4_UNORM_PACK16** = `1`

  4-bit-per-channel red/green/blue/alpha channel data format, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B4G4R4A4_UNORM_PACK16** = `2`

  4-bit-per-channel blue/green/red/alpha channel data format, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R5G6B5_UNORM_PACK16** = `3`

  Red/green/blue channel data format with 5 bits of red, 6 bits of green and 5 bits of blue, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B5G6R5_UNORM_PACK16** = `4`

  Blue/green/red channel data format with 5 bits of blue, 6 bits of green and 5 bits of red, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R5G5B5A1_UNORM_PACK16** = `5`

  Red/green/blue/alpha channel data format with 5 bits of red, 6 bits of green, 5 bits of blue and 1 bit of alpha, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B5G5R5A1_UNORM_PACK16** = `6`

  Blue/green/red/alpha channel data format with 5 bits of blue, 6 bits of green, 5 bits of red and 1 bit of alpha, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_A1R5G5B5_UNORM_PACK16** = `7`

  Alpha/red/green/blue channel data format with 1 bit of alpha, 5 bits of red, 6 bits of green and 5 bits of blue, packed into 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8_UNORM** = `8`

  8-bit-per-channel unsigned floating-point red channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8_SNORM** = `9`

  8-bit-per-channel signed floating-point red channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R8_USCALED** = `10`

  8-bit-per-channel unsigned floating-point red channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_R8_SSCALED** = `11`

  8-bit-per-channel signed floating-point red channel data format with scaled value (value is converted from integer to float). Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_R8_UINT** = `12`

  8-bit-per-channel unsigned integer red channel data format. Values are in the `[0, 255]` range.

- **DATA_FORMAT_R8_SINT** = `13`

  8-bit-per-channel signed integer red channel data format. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_R8_SRGB** = `14`

  8-bit-per-channel unsigned floating-point red channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8G8_UNORM** = `15`

  8-bit-per-channel unsigned floating-point red/green channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8G8_SNORM** = `16`

  8-bit-per-channel signed floating-point red/green channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R8G8_USCALED** = `17`

  8-bit-per-channel unsigned floating-point red/green channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_R8G8_SSCALED** = `18`

  8-bit-per-channel signed floating-point red/green channel data format with scaled value (value is converted from integer to float). Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_R8G8_UINT** = `19`

  8-bit-per-channel unsigned integer red/green channel data format. Values are in the `[0, 255]` range.

- **DATA_FORMAT_R8G8_SINT** = `20`

  8-bit-per-channel signed integer red/green channel data format. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_R8G8_SRGB** = `21`

  8-bit-per-channel unsigned floating-point red/green channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8G8B8_UNORM** = `22`

  8-bit-per-channel unsigned floating-point red/green/blue channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8G8B8_SNORM** = `23`

  8-bit-per-channel signed floating-point red/green/blue channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R8G8B8_USCALED** = `24`

  8-bit-per-channel unsigned floating-point red/green/blue channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_R8G8B8_SSCALED** = `25`

  8-bit-per-channel signed floating-point red/green/blue channel data format with scaled value (value is converted from integer to float). Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_R8G8B8_UINT** = `26`

  8-bit-per-channel unsigned integer red/green/blue channel data format. Values are in the `[0, 255]` range.

- **DATA_FORMAT_R8G8B8_SINT** = `27`

  8-bit-per-channel signed integer red/green/blue channel data format. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_R8G8B8_SRGB** = `28`

  8-bit-per-channel unsigned floating-point red/green/blue channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B8G8R8_UNORM** = `29`

  8-bit-per-channel unsigned floating-point blue/green/red channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B8G8R8_SNORM** = `30`

  8-bit-per-channel signed floating-point blue/green/red channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_B8G8R8_USCALED** = `31`

  8-bit-per-channel unsigned floating-point blue/green/red channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_B8G8R8_SSCALED** = `32`

  8-bit-per-channel signed floating-point blue/green/red channel data format with scaled value (value is converted from integer to float). Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_B8G8R8_UINT** = `33`

  8-bit-per-channel unsigned integer blue/green/red channel data format. Values are in the `[0, 255]` range.

- **DATA_FORMAT_B8G8R8_SINT** = `34`

  8-bit-per-channel signed integer blue/green/red channel data format. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_B8G8R8_SRGB** = `35`

  8-bit-per-channel unsigned floating-point blue/green/red data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8G8B8A8_UNORM** = `36`

  8-bit-per-channel unsigned floating-point red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R8G8B8A8_SNORM** = `37`

  8-bit-per-channel signed floating-point red/green/blue/alpha channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R8G8B8A8_USCALED** = `38`

  8-bit-per-channel unsigned floating-point red/green/blue/alpha channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_R8G8B8A8_SSCALED** = `39`

  8-bit-per-channel signed floating-point red/green/blue/alpha channel data format with scaled value (value is converted from integer to float). Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_R8G8B8A8_UINT** = `40`

  8-bit-per-channel unsigned integer red/green/blue/alpha channel data format. Values are in the `[0, 255]` range.

- **DATA_FORMAT_R8G8B8A8_SINT** = `41`

  8-bit-per-channel signed integer red/green/blue/alpha channel data format. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_R8G8B8A8_SRGB** = `42`

  8-bit-per-channel unsigned floating-point red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B8G8R8A8_UNORM** = `43`

  8-bit-per-channel unsigned floating-point blue/green/red/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_B8G8R8A8_SNORM** = `44`

  8-bit-per-channel signed floating-point blue/green/red/alpha channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_B8G8R8A8_USCALED** = `45`

  8-bit-per-channel unsigned floating-point blue/green/red/alpha channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_B8G8R8A8_SSCALED** = `46`

  8-bit-per-channel signed floating-point blue/green/red/alpha channel data format with scaled value (value is converted from integer to float). Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_B8G8R8A8_UINT** = `47`

  8-bit-per-channel unsigned integer blue/green/red/alpha channel data format. Values are in the `[0, 255]` range.

- **DATA_FORMAT_B8G8R8A8_SINT** = `48`

  8-bit-per-channel signed integer blue/green/red/alpha channel data format. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_B8G8R8A8_SRGB** = `49`

  8-bit-per-channel unsigned floating-point blue/green/red/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_A8B8G8R8_UNORM_PACK32** = `50`

  8-bit-per-channel unsigned floating-point alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_A8B8G8R8_SNORM_PACK32** = `51`

  8-bit-per-channel signed floating-point alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_A8B8G8R8_USCALED_PACK32** = `52`

  8-bit-per-channel unsigned floating-point alpha/red/green/blue channel data format with scaled value (value is converted from integer to float), packed in 32 bits. Values are in the `[0.0, 255.0]` range.

- **DATA_FORMAT_A8B8G8R8_SSCALED_PACK32** = `53`

  8-bit-per-channel signed floating-point alpha/red/green/blue channel data format with scaled value (value is converted from integer to float), packed in 32 bits. Values are in the `[-127.0, 127.0]` range.

- **DATA_FORMAT_A8B8G8R8_UINT_PACK32** = `54`

  8-bit-per-channel unsigned integer alpha/red/green/blue channel data format, packed in 32 bits. Values are in the `[0, 255]` range.

- **DATA_FORMAT_A8B8G8R8_SINT_PACK32** = `55`

  8-bit-per-channel signed integer alpha/red/green/blue channel data format, packed in 32 bits. Values are in the `[-127, 127]` range.

- **DATA_FORMAT_A8B8G8R8_SRGB_PACK32** = `56`

  8-bit-per-channel unsigned floating-point alpha/red/green/blue channel data format with normalized value and nonlinear sRGB encoding, packed in 32 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_A2R10G10B10_UNORM_PACK32** = `57`

  Unsigned floating-point alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of red, 10 bits of green and 10 bits of blue. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_A2R10G10B10_SNORM_PACK32** = `58`

  Signed floating-point alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of red, 10 bits of green and 10 bits of blue. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_A2R10G10B10_USCALED_PACK32** = `59`

  Unsigned floating-point alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of red, 10 bits of green and 10 bits of blue. Values are in the `[0.0, 1023.0]` range for red/green/blue and `[0.0, 3.0]` for alpha.

- **DATA_FORMAT_A2R10G10B10_SSCALED_PACK32** = `60`

  Signed floating-point alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of red, 10 bits of green and 10 bits of blue. Values are in the `[-511.0, 511.0]` range for red/green/blue and `[-1.0, 1.0]` for alpha.

- **DATA_FORMAT_A2R10G10B10_UINT_PACK32** = `61`

  Unsigned integer alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of red, 10 bits of green and 10 bits of blue. Values are in the `[0, 1023]` range for red/green/blue and `[0, 3]` for alpha.

- **DATA_FORMAT_A2R10G10B10_SINT_PACK32** = `62`

  Signed integer alpha/red/green/blue channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of red, 10 bits of green and 10 bits of blue. Values are in the `[-511, 511]` range for red/green/blue and `[-1, 1]` for alpha.

- **DATA_FORMAT_A2B10G10R10_UNORM_PACK32** = `63`

  Unsigned floating-point alpha/blue/green/red channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of blue, 10 bits of green and 10 bits of red. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_A2B10G10R10_SNORM_PACK32** = `64`

  Signed floating-point alpha/blue/green/red channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of blue, 10 bits of green and 10 bits of red. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_A2B10G10R10_USCALED_PACK32** = `65`

  Unsigned floating-point alpha/blue/green/red channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of blue, 10 bits of green and 10 bits of red. Values are in the `[0.0, 1023.0]` range for blue/green/red and `[0.0, 3.0]` for alpha.

- **DATA_FORMAT_A2B10G10R10_SSCALED_PACK32** = `66`

  Signed floating-point alpha/blue/green/red channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of blue, 10 bits of green and 10 bits of red. Values are in the `[-511.0, 511.0]` range for blue/green/red and `[-1.0, 1.0]` for alpha.

- **DATA_FORMAT_A2B10G10R10_UINT_PACK32** = `67`

  Unsigned integer alpha/blue/green/red channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of blue, 10 bits of green and 10 bits of red. Values are in the `[0, 1023]` range for blue/green/red and `[0, 3]` for alpha.

- **DATA_FORMAT_A2B10G10R10_SINT_PACK32** = `68`

  Signed integer alpha/blue/green/red channel data format with normalized value, packed in 32 bits. Format contains 2 bits of alpha, 10 bits of blue, 10 bits of green and 10 bits of red. Values are in the `[-511, 511]` range for blue/green/red and `[-1, 1]` for alpha.

- **DATA_FORMAT_R16_UNORM** = `69`

  16-bit-per-channel unsigned floating-point red channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R16_SNORM** = `70`

  16-bit-per-channel signed floating-point red channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R16_USCALED** = `71`

  16-bit-per-channel unsigned floating-point red channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 65535.0]` range.

- **DATA_FORMAT_R16_SSCALED** = `72`

  16-bit-per-channel signed floating-point red channel data format with scaled value (value is converted from integer to float). Values are in the `[-32767.0, 32767.0]` range.

- **DATA_FORMAT_R16_UINT** = `73`

  16-bit-per-channel unsigned integer red channel data format. Values are in the `[0.0, 65535]` range.

- **DATA_FORMAT_R16_SINT** = `74`

  16-bit-per-channel signed integer red channel data format. Values are in the `[-32767, 32767]` range.

- **DATA_FORMAT_R16_SFLOAT** = `75`

  16-bit-per-channel signed floating-point red channel data format with the value stored as-is.

- **DATA_FORMAT_R16G16_UNORM** = `76`

  16-bit-per-channel unsigned floating-point red/green channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R16G16_SNORM** = `77`

  16-bit-per-channel signed floating-point red/green channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R16G16_USCALED** = `78`

  16-bit-per-channel unsigned floating-point red/green channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 65535.0]` range.

- **DATA_FORMAT_R16G16_SSCALED** = `79`

  16-bit-per-channel signed floating-point red/green channel data format with scaled value (value is converted from integer to float). Values are in the `[-32767.0, 32767.0]` range.

- **DATA_FORMAT_R16G16_UINT** = `80`

  16-bit-per-channel unsigned integer red/green channel data format. Values are in the `[0.0, 65535]` range.

- **DATA_FORMAT_R16G16_SINT** = `81`

  16-bit-per-channel signed integer red/green channel data format. Values are in the `[-32767, 32767]` range.

- **DATA_FORMAT_R16G16_SFLOAT** = `82`

  16-bit-per-channel signed floating-point red/green channel data format with the value stored as-is.

- **DATA_FORMAT_R16G16B16_UNORM** = `83`

  16-bit-per-channel unsigned floating-point red/green/blue channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R16G16B16_SNORM** = `84`

  16-bit-per-channel signed floating-point red/green/blue channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R16G16B16_USCALED** = `85`

  16-bit-per-channel unsigned floating-point red/green/blue channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 65535.0]` range.

- **DATA_FORMAT_R16G16B16_SSCALED** = `86`

  16-bit-per-channel signed floating-point red/green/blue channel data format with scaled value (value is converted from integer to float). Values are in the `[-32767.0, 32767.0]` range.

- **DATA_FORMAT_R16G16B16_UINT** = `87`

  16-bit-per-channel unsigned integer red/green/blue channel data format. Values are in the `[0.0, 65535]` range.

- **DATA_FORMAT_R16G16B16_SINT** = `88`

  16-bit-per-channel signed integer red/green/blue channel data format. Values are in the `[-32767, 32767]` range.

- **DATA_FORMAT_R16G16B16_SFLOAT** = `89`

  16-bit-per-channel signed floating-point red/green/blue channel data format with the value stored as-is.

- **DATA_FORMAT_R16G16B16A16_UNORM** = `90`

  16-bit-per-channel unsigned floating-point red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R16G16B16A16_SNORM** = `91`

  16-bit-per-channel signed floating-point red/green/blue/alpha channel data format with normalized value. Values are in the `[-1.0, 1.0]` range.

- **DATA_FORMAT_R16G16B16A16_USCALED** = `92`

  16-bit-per-channel unsigned floating-point red/green/blue/alpha channel data format with scaled value (value is converted from integer to float). Values are in the `[0.0, 65535.0]` range.

- **DATA_FORMAT_R16G16B16A16_SSCALED** = `93`

  16-bit-per-channel signed floating-point red/green/blue/alpha channel data format with scaled value (value is converted from integer to float). Values are in the `[-32767.0, 32767.0]` range.

- **DATA_FORMAT_R16G16B16A16_UINT** = `94`

  16-bit-per-channel unsigned integer red/green/blue/alpha channel data format. Values are in the `[0.0, 65535]` range.

- **DATA_FORMAT_R16G16B16A16_SINT** = `95`

  16-bit-per-channel signed integer red/green/blue/alpha channel data format. Values are in the `[-32767, 32767]` range.

- **DATA_FORMAT_R16G16B16A16_SFLOAT** = `96`

  16-bit-per-channel signed floating-point red/green/blue/alpha channel data format with the value stored as-is.

- **DATA_FORMAT_R32_UINT** = `97`

  32-bit-per-channel unsigned integer red channel data format. Values are in the `[0, 2^32 - 1]` range.

- **DATA_FORMAT_R32_SINT** = `98`

  32-bit-per-channel signed integer red channel data format. Values are in the `[2^31 + 1, 2^31 - 1]` range.

- **DATA_FORMAT_R32_SFLOAT** = `99`

  32-bit-per-channel signed floating-point red channel data format with the value stored as-is.

- **DATA_FORMAT_R32G32_UINT** = `100`

  32-bit-per-channel unsigned integer red/green channel data format. Values are in the `[0, 2^32 - 1]` range.

- **DATA_FORMAT_R32G32_SINT** = `101`

  32-bit-per-channel signed integer red/green channel data format. Values are in the `[2^31 + 1, 2^31 - 1]` range.

- **DATA_FORMAT_R32G32_SFLOAT** = `102`

  32-bit-per-channel signed floating-point red/green channel data format with the value stored as-is.

- **DATA_FORMAT_R32G32B32_UINT** = `103`

  32-bit-per-channel unsigned integer red/green/blue channel data format. Values are in the `[0, 2^32 - 1]` range.

- **DATA_FORMAT_R32G32B32_SINT** = `104`

  32-bit-per-channel signed integer red/green/blue channel data format. Values are in the `[2^31 + 1, 2^31 - 1]` range.

- **DATA_FORMAT_R32G32B32_SFLOAT** = `105`

  32-bit-per-channel signed floating-point red/green/blue channel data format with the value stored as-is.

- **DATA_FORMAT_R32G32B32A32_UINT** = `106`

  32-bit-per-channel unsigned integer red/green/blue/alpha channel data format. Values are in the `[0, 2^32 - 1]` range.

- **DATA_FORMAT_R32G32B32A32_SINT** = `107`

  32-bit-per-channel signed integer red/green/blue/alpha channel data format. Values are in the `[2^31 + 1, 2^31 - 1]` range.

- **DATA_FORMAT_R32G32B32A32_SFLOAT** = `108`

  32-bit-per-channel signed floating-point red/green/blue/alpha channel data format with the value stored as-is.

- **DATA_FORMAT_R64_UINT** = `109`

  64-bit-per-channel unsigned integer red channel data format. Values are in the `[0, 2^64 - 1]` range.

- **DATA_FORMAT_R64_SINT** = `110`

  64-bit-per-channel signed integer red channel data format. Values are in the `[2^63 + 1, 2^63 - 1]` range.

- **DATA_FORMAT_R64_SFLOAT** = `111`

  64-bit-per-channel signed floating-point red channel data format with the value stored as-is.

- **DATA_FORMAT_R64G64_UINT** = `112`

  64-bit-per-channel unsigned integer red/green channel data format. Values are in the `[0, 2^64 - 1]` range.

- **DATA_FORMAT_R64G64_SINT** = `113`

  64-bit-per-channel signed integer red/green channel data format. Values are in the `[2^63 + 1, 2^63 - 1]` range.

- **DATA_FORMAT_R64G64_SFLOAT** = `114`

  64-bit-per-channel signed floating-point red/green channel data format with the value stored as-is.

- **DATA_FORMAT_R64G64B64_UINT** = `115`

  64-bit-per-channel unsigned integer red/green/blue channel data format. Values are in the `[0, 2^64 - 1]` range.

- **DATA_FORMAT_R64G64B64_SINT** = `116`

  64-bit-per-channel signed integer red/green/blue channel data format. Values are in the `[2^63 + 1, 2^63 - 1]` range.

- **DATA_FORMAT_R64G64B64_SFLOAT** = `117`

  64-bit-per-channel signed floating-point red/green/blue channel data format with the value stored as-is.

- **DATA_FORMAT_R64G64B64A64_UINT** = `118`

  64-bit-per-channel unsigned integer red/green/blue/alpha channel data format. Values are in the `[0, 2^64 - 1]` range.

- **DATA_FORMAT_R64G64B64A64_SINT** = `119`

  64-bit-per-channel signed integer red/green/blue/alpha channel data format. Values are in the `[2^63 + 1, 2^63 - 1]` range.

- **DATA_FORMAT_R64G64B64A64_SFLOAT** = `120`

  64-bit-per-channel signed floating-point red/green/blue/alpha channel data format with the value stored as-is.

- **DATA_FORMAT_B10G11R11_UFLOAT_PACK32** = `121`

  Unsigned floating-point blue/green/red data format with the value stored as-is, packed in 32 bits. The format's precision is 10 bits of blue channel, 11 bits of green channel and 11 bits of red channel.

- **DATA_FORMAT_E5B9G9R9_UFLOAT_PACK32** = `122`

  Unsigned floating-point exposure/blue/green/red data format with the value stored as-is, packed in 32 bits. The format's precision is 5 bits of exposure, 9 bits of blue channel, 9 bits of green channel and 9 bits of red channel.

- **DATA_FORMAT_D16_UNORM** = `123`

  16-bit unsigned floating-point depth data format with normalized value. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_X8_D24_UNORM_PACK32** = `124`

  24-bit unsigned floating-point depth data format with normalized value, plus 8 unused bits, packed in 32 bits. Values for depth are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_D32_SFLOAT** = `125`

  32-bit signed floating-point depth data format with the value stored as-is.

- **DATA_FORMAT_S8_UINT** = `126`

  8-bit unsigned integer stencil data format.

- **DATA_FORMAT_D16_UNORM_S8_UINT** = `127`

  16-bit unsigned floating-point depth data format with normalized value, plus 8 bits of stencil in unsigned integer format. Values for depth are in the `[0.0, 1.0]` range. Values for stencil are in the `[0, 255]` range.

- **DATA_FORMAT_D24_UNORM_S8_UINT** = `128`

  24-bit unsigned floating-point depth data format with normalized value, plus 8 bits of stencil in unsigned integer format. Values for depth are in the `[0.0, 1.0]` range. Values for stencil are in the `[0, 255]` range.

- **DATA_FORMAT_D32_SFLOAT_S8_UINT** = `129`

  32-bit signed floating-point depth data format with the value stored as-is, plus 8 bits of stencil in unsigned integer format. Values for stencil are in the `[0, 255]` range.

- **DATA_FORMAT_BC1_RGB_UNORM_BLOCK** = `130`

  VRAM-compressed unsigned red/green/blue channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel and 5 bits of blue channel. Using BC1 texture compression (also known as S3TC DXT1).

- **DATA_FORMAT_BC1_RGB_SRGB_BLOCK** = `131`

  VRAM-compressed unsigned red/green/blue channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, and 5 bits of blue channel. Using BC1 texture compression (also known as S3TC DXT1).

- **DATA_FORMAT_BC1_RGBA_UNORM_BLOCK** = `132`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, 5 bits of blue channel and 1 bit of alpha channel. Using BC1 texture compression (also known as S3TC DXT1).

- **DATA_FORMAT_BC1_RGBA_SRGB_BLOCK** = `133`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, 5 bits of blue channel, and 1 bit of alpha channel. Using BC1 texture compression (also known as S3TC DXT1).

- **DATA_FORMAT_BC2_UNORM_BLOCK** = `134`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, 5 bits of blue channel and 4 bits of alpha channel. Using BC2 texture compression (also known as S3TC DXT3).

- **DATA_FORMAT_BC2_SRGB_BLOCK** = `135`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, 5 bits of blue channel, and 4 bits of alpha channel. Using BC2 texture compression (also known as S3TC DXT3).

- **DATA_FORMAT_BC3_UNORM_BLOCK** = `136`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, 5 bits of blue channel and 8 bits of alpha channel. Using BC3 texture compression (also known as S3TC DXT5).

- **DATA_FORMAT_BC3_SRGB_BLOCK** = `137`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. The format's precision is 5 bits of red channel, 6 bits of green channel, 5 bits of blue channel, and 8 bits of alpha channel. Using BC3 texture compression (also known as S3TC DXT5).

- **DATA_FORMAT_BC4_UNORM_BLOCK** = `138`

  VRAM-compressed unsigned red channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is 8 bits of red channel. Using BC4 texture compression.

- **DATA_FORMAT_BC4_SNORM_BLOCK** = `139`

  VRAM-compressed signed red channel data format with normalized value. Values are in the `[-1.0, 1.0]` range. The format's precision is 8 bits of red channel. Using BC4 texture compression.

- **DATA_FORMAT_BC5_UNORM_BLOCK** = `140`

  VRAM-compressed unsigned red/green channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is 8 bits of red channel and 8 bits of green channel. Using BC5 texture compression (also known as S3TC RGTC).

- **DATA_FORMAT_BC5_SNORM_BLOCK** = `141`

  VRAM-compressed signed red/green channel data format with normalized value. Values are in the `[-1.0, 1.0]` range. The format's precision is 8 bits of red channel and 8 bits of green channel. Using BC5 texture compression (also known as S3TC RGTC).

- **DATA_FORMAT_BC6H_UFLOAT_BLOCK** = `142`

  VRAM-compressed unsigned red/green/blue channel data format with the floating-point value stored as-is. The format's precision is between 10 and 13 bits for the red/green/blue channels. Using BC6H texture compression (also known as BPTC HDR).

- **DATA_FORMAT_BC6H_SFLOAT_BLOCK** = `143`

  VRAM-compressed signed red/green/blue channel data format with the floating-point value stored as-is. The format's precision is between 10 and 13 bits for the red/green/blue channels. Using BC6H texture compression (also known as BPTC HDR).

- **DATA_FORMAT_BC7_UNORM_BLOCK** = `144`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range. The format's precision is between 4 and 7 bits for the red/green/blue channels and between 0 and 8 bits for the alpha channel. Also known as BPTC LDR.

- **DATA_FORMAT_BC7_SRGB_BLOCK** = `145`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. The format's precision is between 4 and 7 bits for the red/green/blue channels and between 0 and 8 bits for the alpha channel. Also known as BPTC LDR.

- **DATA_FORMAT_ETC2_R8G8B8_UNORM_BLOCK** = `146`

  VRAM-compressed unsigned red/green/blue channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Using ETC2 texture compression.

- **DATA_FORMAT_ETC2_R8G8B8_SRGB_BLOCK** = `147`

  VRAM-compressed unsigned red/green/blue channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. Using ETC2 texture compression.

- **DATA_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK** = `148`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Red/green/blue use 8 bit of precision each, with alpha using 1 bit of precision. Using ETC2 texture compression.

- **DATA_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK** = `149`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. Red/green/blue use 8 bit of precision each, with alpha using 1 bit of precision. Using ETC2 texture compression.

- **DATA_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK** = `150`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Red/green/blue use 8 bits of precision each, with alpha using 8 bits of precision. Using ETC2 texture compression.

- **DATA_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK** = `151`

  VRAM-compressed unsigned red/green/blue/alpha channel data format with normalized value and nonlinear sRGB encoding. Values are in the `[0.0, 1.0]` range. Red/green/blue use 8 bits of precision each, with alpha using 8 bits of precision. Using ETC2 texture compression.

- **DATA_FORMAT_EAC_R11_UNORM_BLOCK** = `152`

  11-bit VRAM-compressed unsigned red channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Using ETC2 texture compression.

- **DATA_FORMAT_EAC_R11_SNORM_BLOCK** = `153`

  11-bit VRAM-compressed signed red channel data format with normalized value. Values are in the `[-1.0, 1.0]` range. Using ETC2 texture compression.

- **DATA_FORMAT_EAC_R11G11_UNORM_BLOCK** = `154`

  11-bit VRAM-compressed unsigned red/green channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Using ETC2 texture compression.

- **DATA_FORMAT_EAC_R11G11_SNORM_BLOCK** = `155`

  11-bit VRAM-compressed signed red/green channel data format with normalized value. Values are in the `[-1.0, 1.0]` range. Using ETC2 texture compression.

- **DATA_FORMAT_ASTC_4x4_UNORM_BLOCK** = `156`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 4×4 blocks (highest quality). Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_4x4_SRGB_BLOCK** = `157`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 4×4 blocks (highest quality). Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_5x4_UNORM_BLOCK** = `158`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 5×4 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_5x4_SRGB_BLOCK** = `159`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 5×4 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_5x5_UNORM_BLOCK** = `160`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 5×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_5x5_SRGB_BLOCK** = `161`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 5×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_6x5_UNORM_BLOCK** = `162`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 6×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_6x5_SRGB_BLOCK** = `163`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 6×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_6x6_UNORM_BLOCK** = `164`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 6×6 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_6x6_SRGB_BLOCK** = `165`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 6×6 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_8x5_UNORM_BLOCK** = `166`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 8×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_8x5_SRGB_BLOCK** = `167`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 8×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_8x6_UNORM_BLOCK** = `168`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 8×6 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_8x6_SRGB_BLOCK** = `169`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 8×6 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_8x8_UNORM_BLOCK** = `170`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 8×8 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_8x8_SRGB_BLOCK** = `171`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 8×8 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x5_UNORM_BLOCK** = `172`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 10×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x5_SRGB_BLOCK** = `173`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 10×5 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x6_UNORM_BLOCK** = `174`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 10×6 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x6_SRGB_BLOCK** = `175`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 10×6 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x8_UNORM_BLOCK** = `176`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 10×8 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x8_SRGB_BLOCK** = `177`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 10×8 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x10_UNORM_BLOCK** = `178`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 10×10 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_10x10_SRGB_BLOCK** = `179`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 10×10 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_12x10_UNORM_BLOCK** = `180`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 12×10 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_12x10_SRGB_BLOCK** = `181`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 12×10 blocks. Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_12x12_UNORM_BLOCK** = `182`

  VRAM-compressed unsigned floating-point data format with normalized value, packed in 12 blocks (lowest quality). Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_ASTC_12x12_SRGB_BLOCK** = `183`

  VRAM-compressed unsigned floating-point data format with normalized value and nonlinear sRGB encoding, packed in 12 blocks (lowest quality). Values are in the `[0.0, 1.0]` range. Using ASTC compression.

- **DATA_FORMAT_G8B8G8R8_422_UNORM** = `184`

  8-bit-per-channel unsigned floating-point green/blue/red channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_B8G8R8G8_422_UNORM** = `185`

  8-bit-per-channel unsigned floating-point blue/green/red channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G8_B8_R8_3PLANE_420_UNORM** = `186`

  8-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G8_B8R8_2PLANE_420_UNORM** = `187`

  8-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, stored across 2 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G8_B8_R8_3PLANE_422_UNORM** = `188`

  8-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, stored across 2 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G8_B8R8_2PLANE_422_UNORM** = `189`

  8-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, stored across 2 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G8_B8_R8_3PLANE_444_UNORM** = `190`

  8-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, stored across 3 separate planes. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R10X6_UNORM_PACK16** = `191`

  10-bit-per-channel unsigned floating-point red channel data with normalized value, plus 6 unused bits, packed in 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R10X6G10X6_UNORM_2PACK16** = `192`

  10-bit-per-channel unsigned floating-point red/green channel data with normalized value, plus 6 unused bits after each channel, packed in 2×16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R10X6G10X6B10X6A10X6_UNORM_4PACK16** = `193`

  10-bit-per-channel unsigned floating-point red/green/blue/alpha channel data with normalized value, plus 6 unused bits after each channel, packed in 4×16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_G10X6B10X6G10X6R10X6_422_UNORM_4PACK16** = `194`

  10-bit-per-channel unsigned floating-point green/blue/green/red channel data with normalized value, plus 6 unused bits after each channel, packed in 4×16 bits. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel). The green channel is listed twice, but contains different values to allow it to be represented at full resolution.

- **DATA_FORMAT_B10X6G10X6R10X6G10X6_422_UNORM_4PACK16** = `195`

  10-bit-per-channel unsigned floating-point blue/green/red/green channel data with normalized value, plus 6 unused bits after each channel, packed in 4×16 bits. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel). The green channel is listed twice, but contains different values to allow it to be represented at full resolution.

- **DATA_FORMAT_G10X6_B10X6_R10X6_3PLANE_420_UNORM_3PACK16** = `196`

  10-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 2 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16** = `197`

  10-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 2 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G10X6_B10X6_R10X6_3PLANE_422_UNORM_3PACK16** = `198`

  10-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G10X6_B10X6R10X6_2PLANE_422_UNORM_3PACK16** = `199`

  10-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 3 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G10X6_B10X6_R10X6_3PLANE_444_UNORM_3PACK16** = `200`

  10-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R12X4_UNORM_PACK16** = `201`

  12-bit-per-channel unsigned floating-point red channel data with normalized value, plus 6 unused bits, packed in 16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R12X4G12X4_UNORM_2PACK16** = `202`

  12-bit-per-channel unsigned floating-point red/green channel data with normalized value, plus 6 unused bits after each channel, packed in 2×16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_R12X4G12X4B12X4A12X4_UNORM_4PACK16** = `203`

  12-bit-per-channel unsigned floating-point red/green/blue/alpha channel data with normalized value, plus 6 unused bits after each channel, packed in 4×16 bits. Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_G12X4B12X4G12X4R12X4_422_UNORM_4PACK16** = `204`

  12-bit-per-channel unsigned floating-point green/blue/green/red channel data with normalized value, plus 6 unused bits after each channel, packed in 4×16 bits. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel). The green channel is listed twice, but contains different values to allow it to be represented at full resolution.

- **DATA_FORMAT_B12X4G12X4R12X4G12X4_422_UNORM_4PACK16** = `205`

  12-bit-per-channel unsigned floating-point blue/green/red/green channel data with normalized value, plus 6 unused bits after each channel, packed in 4×16 bits. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel). The green channel is listed twice, but contains different values to allow it to be represented at full resolution.

- **DATA_FORMAT_G12X4_B12X4_R12X4_3PLANE_420_UNORM_3PACK16** = `206`

  12-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 2 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G12X4_B12X4R12X4_2PLANE_420_UNORM_3PACK16** = `207`

  12-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 2 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G12X4_B12X4_R12X4_3PLANE_422_UNORM_3PACK16** = `208`

  12-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G12X4_B12X4R12X4_2PLANE_422_UNORM_3PACK16** = `209`

  12-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 3 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G12X4_B12X4_R12X4_3PLANE_444_UNORM_3PACK16** = `210`

  12-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Packed in 3×16 bits and stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_G16B16G16R16_422_UNORM** = `211`

  16-bit-per-channel unsigned floating-point green/blue/red channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_B16G16R16G16_422_UNORM** = `212`

  16-bit-per-channel unsigned floating-point blue/green/red channel data format with normalized value. Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G16_B16_R16_3PLANE_420_UNORM** = `213`

  16-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Stored across 2 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G16_B16R16_2PLANE_420_UNORM** = `214`

  16-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Stored across 2 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal and vertical resolution (i.e. 2×2 adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G16_B16_R16_3PLANE_422_UNORM** = `215`

  16-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G16_B16R16_2PLANE_422_UNORM** = `216`

  16-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Stored across 3 separate planes (green + blue/red). Values are in the `[0.0, 1.0]` range. Blue and red channel data is stored at halved horizontal resolution (i.e. 2 horizontally adjacent pixels will share the same value for the blue/red channel).

- **DATA_FORMAT_G16_B16_R16_3PLANE_444_UNORM** = `217`

  16-bit-per-channel unsigned floating-point green/blue/red channel data with normalized value, plus 6 unused bits after each channel. Stored across 3 separate planes (green + blue + red). Values are in the `[0.0, 1.0]` range.

- **DATA_FORMAT_ASTC_4x4_SFLOAT_BLOCK** = `218`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_5x4_SFLOAT_BLOCK** = `219`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_5x5_SFLOAT_BLOCK** = `220`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_6x5_SFLOAT_BLOCK** = `221`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_6x6_SFLOAT_BLOCK** = `222`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_8x5_SFLOAT_BLOCK** = `223`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_8x6_SFLOAT_BLOCK** = `224`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_8x8_SFLOAT_BLOCK** = `225`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_10x5_SFLOAT_BLOCK** = `226`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_10x6_SFLOAT_BLOCK** = `227`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_10x8_SFLOAT_BLOCK** = `228`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_10x10_SFLOAT_BLOCK** = `229`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_12x10_SFLOAT_BLOCK** = `230`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_ASTC_12x12_SFLOAT_BLOCK** = `231`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DATA_FORMAT_MAX** = `232`

  Represents the size of the [DataFormat](#enum-dataformat) enum.

### flags BarrierMask

- **BARRIER_MASK_VERTEX** = `1`

  Vertex shader barrier mask.

- **BARRIER_MASK_FRAGMENT** = `8`

  Fragment shader barrier mask.

- **BARRIER_MASK_COMPUTE** = `2`

  Compute barrier mask.

- **BARRIER_MASK_TRANSFER** = `4`

  Transfer barrier mask.

- **BARRIER_MASK_RASTER** = `9`

  Raster barrier mask (vertex and fragment). Equivalent to `BARRIER_MASK_VERTEX | BARRIER_MASK_FRAGMENT`.

- **BARRIER_MASK_ALL_BARRIERS** = `32767`

  Barrier mask for all types (vertex, fragment, compute, transfer).

- **BARRIER_MASK_NO_BARRIER** = `32768`

  No barrier for any type.

### enum TextureType

- **TEXTURE_TYPE_1D** = `0`

  1-dimensional texture.

- **TEXTURE_TYPE_2D** = `1`

  2-dimensional texture.

- **TEXTURE_TYPE_3D** = `2`

  3-dimensional texture.

- **TEXTURE_TYPE_CUBE** = `3`

  [Cubemap](https://docs.godotengine.org/en/stable/classes/class_cubemap.html) texture.

- **TEXTURE_TYPE_1D_ARRAY** = `4`

  Array of 1-dimensional textures.

- **TEXTURE_TYPE_2D_ARRAY** = `5`

  Array of 2-dimensional textures.

- **TEXTURE_TYPE_CUBE_ARRAY** = `6`

  Array of [Cubemap](https://docs.godotengine.org/en/stable/classes/class_cubemap.html) textures.

- **TEXTURE_TYPE_MAX** = `7`

  Represents the size of the [TextureType](#enum-texturetype) enum.

### enum TextureSamples

- **TEXTURE_SAMPLES_1** = `0`

  Perform 1 texture sample (this is the fastest but lowest-quality for antialiasing).

- **TEXTURE_SAMPLES_2** = `1`

  Perform 2 texture samples.

- **TEXTURE_SAMPLES_4** = `2`

  Perform 4 texture samples.

- **TEXTURE_SAMPLES_8** = `3`

  Perform 8 texture samples. Not supported on mobile GPUs (including Apple Silicon).

- **TEXTURE_SAMPLES_16** = `4`

  Perform 16 texture samples. Not supported on mobile GPUs and many desktop GPUs.

- **TEXTURE_SAMPLES_32** = `5`

  Perform 32 texture samples. Not supported on most GPUs.

- **TEXTURE_SAMPLES_64** = `6`

  Perform 64 texture samples (this is the slowest but highest-quality for antialiasing). Not supported on most GPUs.

- **TEXTURE_SAMPLES_MAX** = `7`

  Represents the size of the [TextureSamples](#enum-texturesamples) enum.

### flags TextureUsageBits

- **TEXTURE_USAGE_SAMPLING_BIT** = `1`

  Texture can be sampled.

- **TEXTURE_USAGE_COLOR_ATTACHMENT_BIT** = `2`

  Texture can be used as a color attachment in a framebuffer.

- **TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT** = `4`

  Texture can be used as a depth/stencil attachment in a framebuffer.

- **TEXTURE_USAGE_DEPTH_RESOLVE_ATTACHMENT_BIT** = `4096`

  Texture can be used as a depth/stencil resolve attachment in a framebuffer.

- **TEXTURE_USAGE_STORAGE_BIT** = `8`

  Texture can be used as a [storage image](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#descriptorsets-storageimage).

- **TEXTURE_USAGE_STORAGE_ATOMIC_BIT** = `16`

  Texture can be used as a [storage image](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#descriptorsets-storageimage) with support for atomic operations.

- **TEXTURE_USAGE_CPU_READ_BIT** = `32`

  Texture can be read back on the CPU using [texture_get_data()](#texture_get_data) faster than without this bit, since it is always kept in the system memory.

- **TEXTURE_USAGE_CAN_UPDATE_BIT** = `64`

  Texture can be updated using [texture_update()](#texture_update).

- **TEXTURE_USAGE_CAN_COPY_FROM_BIT** = `128`

  Texture can be a source for [texture_copy()](#texture_copy).

- **TEXTURE_USAGE_CAN_COPY_TO_BIT** = `256`

  Texture can be a destination for [texture_copy()](#texture_copy).

- **TEXTURE_USAGE_INPUT_ATTACHMENT_BIT** = `512`

  Texture can be used as a [input attachment](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#descriptorsets-inputattachment) in a framebuffer.

### enum TextureSwizzle

- **TEXTURE_SWIZZLE_IDENTITY** = `0`

  Return the sampled value as-is.

- **TEXTURE_SWIZZLE_ZERO** = `1`

  Always return `0.0` when sampling.

- **TEXTURE_SWIZZLE_ONE** = `2`

  Always return `1.0` when sampling.

- **TEXTURE_SWIZZLE_R** = `3`

  Sample the red color channel.

- **TEXTURE_SWIZZLE_G** = `4`

  Sample the green color channel.

- **TEXTURE_SWIZZLE_B** = `5`

  Sample the blue color channel.

- **TEXTURE_SWIZZLE_A** = `6`

  Sample the alpha channel.

- **TEXTURE_SWIZZLE_MAX** = `7`

  Represents the size of the [TextureSwizzle](#enum-textureswizzle) enum.

### enum TextureSliceType

- **TEXTURE_SLICE_2D** = `0`

  2-dimensional texture slice.

- **TEXTURE_SLICE_CUBEMAP** = `1`

  Cubemap texture slice.

- **TEXTURE_SLICE_3D** = `2`

  3-dimensional texture slice.

### enum SamplerFilter

- **SAMPLER_FILTER_NEAREST** = `0`

  Nearest-neighbor sampler filtering. Sampling at higher resolutions than the source will result in a pixelated look.

- **SAMPLER_FILTER_LINEAR** = `1`

  Bilinear sampler filtering. Sampling at higher resolutions than the source will result in a blurry look.

### enum SamplerRepeatMode

- **SAMPLER_REPEAT_MODE_REPEAT** = `0`

  Sample with repeating enabled.

- **SAMPLER_REPEAT_MODE_MIRRORED_REPEAT** = `1`

  Sample with mirrored repeating enabled. When sampling outside the `[0.0, 1.0]` range, return a mirrored version of the sampler. This mirrored version is mirrored again if sampling further away, with the pattern repeating indefinitely.

- **SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE** = `2`

  Sample with repeating disabled. When sampling outside the `[0.0, 1.0]` range, return the color of the last pixel on the edge.

- **SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER** = `3`

  Sample with repeating disabled. When sampling outside the `[0.0, 1.0]` range, return the specified [RDSamplerState.border_color](https://docs.godotengine.org/en/stable/classes/class_rdsamplerstate.html#class_rdsamplerstate_property_border_color).

- **SAMPLER_REPEAT_MODE_MIRROR_CLAMP_TO_EDGE** = `4`

  Sample with mirrored repeating enabled, but only once. When sampling in the `[-1.0, 0.0]` range, return a mirrored version of the sampler. When sampling outside the `[-1.0, 1.0]` range, return the color of the last pixel on the edge.

- **SAMPLER_REPEAT_MODE_MAX** = `5`

  Represents the size of the [SamplerRepeatMode](#enum-samplerrepeatmode) enum.

### enum SamplerBorderColor

- **SAMPLER_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK** = `0`

  Return a floating-point transparent black color when sampling outside the `[0.0, 1.0]` range. Only effective if the sampler repeat mode is [SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_sampler_repeat_mode_clamp_to_border).

- **SAMPLER_BORDER_COLOR_INT_TRANSPARENT_BLACK** = `1`

  Return an integer transparent black color when sampling outside the `[0.0, 1.0]` range. Only effective if the sampler repeat mode is [SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_sampler_repeat_mode_clamp_to_border).

- **SAMPLER_BORDER_COLOR_FLOAT_OPAQUE_BLACK** = `2`

  Return a floating-point opaque black color when sampling outside the `[0.0, 1.0]` range. Only effective if the sampler repeat mode is [SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_sampler_repeat_mode_clamp_to_border).

- **SAMPLER_BORDER_COLOR_INT_OPAQUE_BLACK** = `3`

  Return an integer opaque black color when sampling outside the `[0.0, 1.0]` range. Only effective if the sampler repeat mode is [SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_sampler_repeat_mode_clamp_to_border).

- **SAMPLER_BORDER_COLOR_FLOAT_OPAQUE_WHITE** = `4`

  Return a floating-point opaque white color when sampling outside the `[0.0, 1.0]` range. Only effective if the sampler repeat mode is [SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_sampler_repeat_mode_clamp_to_border).

- **SAMPLER_BORDER_COLOR_INT_OPAQUE_WHITE** = `5`

  Return an integer opaque white color when sampling outside the `[0.0, 1.0]` range. Only effective if the sampler repeat mode is [SAMPLER_REPEAT_MODE_CLAMP_TO_BORDER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_sampler_repeat_mode_clamp_to_border).

- **SAMPLER_BORDER_COLOR_MAX** = `6`

  Represents the size of the [SamplerBorderColor](#enum-samplerbordercolor) enum.

### enum VertexFrequency

- **VERTEX_FREQUENCY_VERTEX** = `0`

  Vertex attribute addressing is a function of the vertex. This is used to specify the rate at which vertex attributes are pulled from buffers.

- **VERTEX_FREQUENCY_INSTANCE** = `1`

  Vertex attribute addressing is a function of the instance index. This is used to specify the rate at which vertex attributes are pulled from buffers.

### enum IndexBufferFormat

- **INDEX_BUFFER_FORMAT_UINT16** = `0`

  Index buffer in 16-bit unsigned integer format. This limits the maximum index that can be specified to `65535`.

- **INDEX_BUFFER_FORMAT_UINT32** = `1`

  Index buffer in 32-bit unsigned integer format. This limits the maximum index that can be specified to `4294967295`.

### flags StorageBufferUsage

- **STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT** = `1`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

### flags BufferCreationBits

- **BUFFER_CREATION_DEVICE_ADDRESS_BIT** = `1`

  Optionally, set this flag if you wish to use [buffer_get_device_address()](#buffer_get_device_address) functionality. You must first check the GPU supports it:

  ```gdscript
  rd = RenderingServer.get_rendering_device()
 
  if rd.has_feature(RenderingDevice.SUPPORTS_BUFFER_DEVICE_ADDRESS):
      storage_buffer = rd.storage_buffer_create(bytes.size(), bytes, RenderingDevice.STORAGE_BUFFER_USAGE_SHADER_DEVICE_ADDRESS)
      storage_buffer_address = rd.buffer_get_device_address(storage_buffer)
  ```

- **BUFFER_CREATION_AS_STORAGE_BIT** = `2`

  Set this flag so that it is created as storage. This is useful if Compute Shaders need access (for reading or writing) to the buffer, e.g. skeletal animations are processed in Compute Shaders which need access to vertex buffers, to be later consumed by vertex shaders as part of the regular rasterization pipeline.

- **BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT** = `8`

  **Experimental:** This constant may be changed or removed in future versions.

  Allows usage of this buffer as input data for an acceleration structure build operation. You must first check that the GPU supports it:

  ```gdscript
  rd = RenderingServer.get_rendering_device()
 
  if rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE):
      storage_buffer = rd.storage_buffer_create(bytes.size(), bytes, RenderingDevice.BUFFER_CREATION_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT)
  ```

### flags AccelerationStructureFlagBits

- **ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT** = `1`

  **Experimental:** This constant may be changed or removed in future versions.

  Allows the acceleration structure to be updated after it has been built.

- **ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT** = `2`

  **Experimental:** This constant may be changed or removed in future versions.

  Allows the acceleration structure to be compacted to reduce memory usage after it has been built.

- **ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT** = `4`

  **Experimental:** This constant may be changed or removed in future versions.

  Prioritizes ray traversal performance over build performance when building the acceleration structure.

- **ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT** = `8`

  **Experimental:** This constant may be changed or removed in future versions.

  Prioritizes build performance over ray traversal performance when building the acceleration structure.

- **ACCELERATION_STRUCTURE_LOW_MEMORY_BIT** = `16`

  **Experimental:** This constant may be changed or removed in future versions.

  Reduces the memory usage of the acceleration structure, potentially at the cost of reduced ray traversal performance.

### flags AccelerationStructureGeometryFlagBits

- **ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT** = `1`

  **Experimental:** This constant may be changed or removed in future versions.

  An opaque geometry does not invoke the any hit shaders.

- **ACCELERATION_STRUCTURE_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT** = `2`

  **Experimental:** This constant may be changed or removed in future versions.

  This geometry only calls the any hit shader a single time for each primitive.

### flags AccelerationStructureInstanceFlagBits

- **ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT** = `1`

  **Experimental:** This constant may be changed or removed in future versions.

  Disables triangle face culling for this instance during ray traversal.

- **ACCELERATION_STRUCTURE_INSTANCE_TRIANGLE_FLIP_FACING_BIT** = `2`

  **Experimental:** This constant may be changed or removed in future versions.

  Flips the triangle facing direction for this instance during ray traversal.

- **ACCELERATION_STRUCTURE_INSTANCE_FORCE_OPAQUE_BIT** = `4`

  **Experimental:** This constant may be changed or removed in future versions.

  Forces all geometries in this instance to be treated as opaque, preventing any hit shaders from being invoked.

- **ACCELERATION_STRUCTURE_INSTANCE_FORCE_NO_OPAQUE_BIT** = `8`

  **Experimental:** This constant may be changed or removed in future versions.

  Forces all geometries in this instance to be treated as non-opaque, allowing any hit shaders to be invoked.

### enum UniformType

- **UNIFORM_TYPE_SAMPLER** = `0`

  Sampler uniform.

- **UNIFORM_TYPE_SAMPLER_WITH_TEXTURE** = `1`

  Sampler uniform with a texture.

- **UNIFORM_TYPE_TEXTURE** = `2`

  Texture uniform.

- **UNIFORM_TYPE_IMAGE** = `3`

  Image uniform.

- **UNIFORM_TYPE_TEXTURE_BUFFER** = `4`

  Texture buffer uniform.

- **UNIFORM_TYPE_SAMPLER_WITH_TEXTURE_BUFFER** = `5`

  Sampler uniform with a texture buffer.

- **UNIFORM_TYPE_IMAGE_BUFFER** = `6`

  Image buffer uniform.

- **UNIFORM_TYPE_UNIFORM_BUFFER** = `7`

  Uniform buffer uniform.

- **UNIFORM_TYPE_STORAGE_BUFFER** = `8`

  [Storage buffer](https://vkguide.dev/docs/chapter-4/storage_buffers/) uniform.

- **UNIFORM_TYPE_INPUT_ATTACHMENT** = `9`

  Input attachment uniform.

- **UNIFORM_TYPE_UNIFORM_BUFFER_DYNAMIC** = `10`

  Same as UNIFORM_TYPE_UNIFORM_BUFFER but for buffers created with BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT.

  **Note:** This flag is not available to GD users due to being too dangerous (i.e. wrong usage can result in visual glitches).

  It's exposed in case GD users receive a buffer created with such flag from Godot.

- **UNIFORM_TYPE_STORAGE_BUFFER_DYNAMIC** = `11`

  Same as UNIFORM_TYPE_STORAGE_BUFFER but for buffers created with BUFFER_CREATION_DYNAMIC_PERSISTENT_BIT.

  **Note:** This flag is not available to GD users due to being too dangerous (i.e. wrong usage can result in visual glitches).

  It's exposed in case GD users receive a buffer created with such flag from Godot.

- **UNIFORM_TYPE_ACCELERATION_STRUCTURE** = `12`

  Acceleration structure uniform.

- **UNIFORM_TYPE_MAX** = `13`

  Represents the size of the [UniformType](#enum-uniformtype) enum.

### enum RenderPrimitive

- **RENDER_PRIMITIVE_POINTS** = `0`

  Point rendering primitive (with constant size, regardless of distance from camera).

- **RENDER_PRIMITIVE_LINES** = `1`

  Line list rendering primitive. Lines are drawn separated from each other.

- **RENDER_PRIMITIVE_LINES_WITH_ADJACENCY** = `2`

  [Line list rendering primitive with adjacency.](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#drawing-line-lists-with-adjacency)\

  **Note:** Adjacency is only useful with geometry shaders, which Godot does not expose.

- **RENDER_PRIMITIVE_LINESTRIPS** = `3`

  Line strip rendering primitive. Lines drawn are connected to the previous vertex.

- **RENDER_PRIMITIVE_LINESTRIPS_WITH_ADJACENCY** = `4`

  [Line strip rendering primitive with adjacency.](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#drawing-line-strips-with-adjacency)\

  **Note:** Adjacency is only useful with geometry shaders, which Godot does not expose.

- **RENDER_PRIMITIVE_TRIANGLES** = `5`

  Triangle list rendering primitive. Triangles are drawn separated from each other.

- **RENDER_PRIMITIVE_TRIANGLES_WITH_ADJACENCY** = `6`

  [Triangle list rendering primitive with adjacency.](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#drawing-triangle-lists-with-adjacency)\

  **Note:** Adjacency is only useful with geometry shaders, which Godot does not expose.

- **RENDER_PRIMITIVE_TRIANGLE_STRIPS** = `7`

  Triangle strip rendering primitive. Triangles drawn are connected to the previous triangle.

- **RENDER_PRIMITIVE_TRIANGLE_STRIPS_WITH_AJACENCY** = `8`

  [Triangle strip rendering primitive with adjacency.](https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#drawing-triangle-strips-with-adjacency)\

  **Note:** Adjacency is only useful with geometry shaders, which Godot does not expose.

- **RENDER_PRIMITIVE_TRIANGLE_STRIPS_WITH_RESTART_INDEX** = `9`

  Triangle strip rendering primitive with *primitive restart* enabled. Triangles drawn are connected to the previous triangle, but a primitive restart index can be specified before drawing to create a second triangle strip after the specified index.

  **Note:** Only compatible with indexed draws.

- **RENDER_PRIMITIVE_TESSELATION_PATCH** = `10`

  Tessellation patch rendering primitive. Only useful with tessellation shaders, which can be used to deform these patches.

- **RENDER_PRIMITIVE_MAX** = `11`

  Represents the size of the [RenderPrimitive](#enum-renderprimitive) enum.

### enum PolygonCullMode

- **POLYGON_CULL_DISABLED** = `0`

  Do not use polygon front face or backface culling.

- **POLYGON_CULL_FRONT** = `1`

  Use polygon frontface culling (faces pointing towards the camera are hidden).

- **POLYGON_CULL_BACK** = `2`

  Use polygon backface culling (faces pointing away from the camera are hidden).

### enum PolygonFrontFace

- **POLYGON_FRONT_FACE_CLOCKWISE** = `0`

  Clockwise winding order to determine which face of a polygon is its front face.

- **POLYGON_FRONT_FACE_COUNTER_CLOCKWISE** = `1`

  Counter-clockwise winding order to determine which face of a polygon is its front face.

### enum StencilOperation

- **STENCIL_OP_KEEP** = `0`

  Keep the current stencil value.

- **STENCIL_OP_ZERO** = `1`

  Set the stencil value to `0`.

- **STENCIL_OP_REPLACE** = `2`

  Replace the existing stencil value with the new one.

- **STENCIL_OP_INCREMENT_AND_CLAMP** = `3`

  Increment the existing stencil value and clamp to the maximum representable unsigned value if reached. Stencil bits are considered as an unsigned integer.

- **STENCIL_OP_DECREMENT_AND_CLAMP** = `4`

  Decrement the existing stencil value and clamp to the minimum value if reached. Stencil bits are considered as an unsigned integer.

- **STENCIL_OP_INVERT** = `5`

  Bitwise-invert the existing stencil value.

- **STENCIL_OP_INCREMENT_AND_WRAP** = `6`

  Increment the stencil value and wrap around to `0` if reaching the maximum representable unsigned. Stencil bits are considered as an unsigned integer.

- **STENCIL_OP_DECREMENT_AND_WRAP** = `7`

  Decrement the stencil value and wrap around to the maximum representable unsigned if reaching the minimum. Stencil bits are considered as an unsigned integer.

- **STENCIL_OP_MAX** = `8`

  Represents the size of the [StencilOperation](#enum-stenciloperation) enum.

### enum CompareOperator

- **COMPARE_OP_NEVER** = `0`

  "Never" comparison (opposite of [COMPARE_OP_ALWAYS](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_compare_op_always)).

- **COMPARE_OP_LESS** = `1`

  "Less than" comparison.

- **COMPARE_OP_EQUAL** = `2`

  "Equal" comparison.

- **COMPARE_OP_LESS_OR_EQUAL** = `3`

  "Less than or equal" comparison.

- **COMPARE_OP_GREATER** = `4`

  "Greater than" comparison.

- **COMPARE_OP_NOT_EQUAL** = `5`

  "Not equal" comparison.

- **COMPARE_OP_GREATER_OR_EQUAL** = `6`

  "Greater than or equal" comparison.

- **COMPARE_OP_ALWAYS** = `7`

  "Always" comparison (opposite of [COMPARE_OP_NEVER](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_compare_op_never)).

- **COMPARE_OP_MAX** = `8`

  Represents the size of the [CompareOperator](#enum-compareoperator) enum.

### enum LogicOperation

- **LOGIC_OP_CLEAR** = `0`

  Clear logic operation (result is always `0`). See also [LOGIC_OP_SET](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_set).

- **LOGIC_OP_AND** = `1`

  AND logic operation.

- **LOGIC_OP_AND_REVERSE** = `2`

  AND logic operation with the *destination* operand being inverted. See also [LOGIC_OP_AND_INVERTED](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_and_inverted).

- **LOGIC_OP_COPY** = `3`

  Copy logic operation (keeps the *source* value as-is). See also [LOGIC_OP_COPY_INVERTED](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_copy_inverted) and [LOGIC_OP_NO_OP](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_no_op).

- **LOGIC_OP_AND_INVERTED** = `4`

  AND logic operation with the *source* operand being inverted. See also [LOGIC_OP_AND_REVERSE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_and_reverse).

- **LOGIC_OP_NO_OP** = `5`

  No-op logic operation (keeps the *destination* value as-is). See also [LOGIC_OP_COPY](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_copy).

- **LOGIC_OP_XOR** = `6`

  Exclusive or (XOR) logic operation.

- **LOGIC_OP_OR** = `7`

  OR logic operation.

- **LOGIC_OP_NOR** = `8`

  Not-OR (NOR) logic operation.

- **LOGIC_OP_EQUIVALENT** = `9`

  Not-XOR (XNOR) logic operation.

- **LOGIC_OP_INVERT** = `10`

  Invert logic operation.

- **LOGIC_OP_OR_REVERSE** = `11`

  OR logic operation with the *destination* operand being inverted. See also [LOGIC_OP_OR_REVERSE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_or_reverse).

- **LOGIC_OP_COPY_INVERTED** = `12`

  NOT logic operation (inverts the value). See also [LOGIC_OP_COPY](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_copy).

- **LOGIC_OP_OR_INVERTED** = `13`

  OR logic operation with the *source* operand being inverted. See also [LOGIC_OP_OR_REVERSE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_or_reverse).

- **LOGIC_OP_NAND** = `14`

  Not-AND (NAND) logic operation.

- **LOGIC_OP_SET** = `15`

  SET logic operation (result is always `1`). See also [LOGIC_OP_CLEAR](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_logic_op_clear).

- **LOGIC_OP_MAX** = `16`

  Represents the size of the [LogicOperation](#enum-logicoperation) enum.

### enum BlendFactor

- **BLEND_FACTOR_ZERO** = `0`

  Constant `0.0` blend factor.

- **BLEND_FACTOR_ONE** = `1`

  Constant `1.0` blend factor.

- **BLEND_FACTOR_SRC_COLOR** = `2`

  Color blend factor is `source color`. Alpha blend factor is `source alpha`.

- **BLEND_FACTOR_ONE_MINUS_SRC_COLOR** = `3`

  Color blend factor is `1.0 - source color`. Alpha blend factor is `1.0 - source alpha`.

- **BLEND_FACTOR_DST_COLOR** = `4`

  Color blend factor is `destination color`. Alpha blend factor is `destination alpha`.

- **BLEND_FACTOR_ONE_MINUS_DST_COLOR** = `5`

  Color blend factor is `1.0 - destination color`. Alpha blend factor is `1.0 - destination alpha`.

- **BLEND_FACTOR_SRC_ALPHA** = `6`

  Color and alpha blend factor is `source alpha`.

- **BLEND_FACTOR_ONE_MINUS_SRC_ALPHA** = `7`

  Color and alpha blend factor is `1.0 - source alpha`.

- **BLEND_FACTOR_DST_ALPHA** = `8`

  Color and alpha blend factor is `destination alpha`.

- **BLEND_FACTOR_ONE_MINUS_DST_ALPHA** = `9`

  Color and alpha blend factor is `1.0 - destination alpha`.

- **BLEND_FACTOR_CONSTANT_COLOR** = `10`

  Color blend factor is `blend constant color`. Alpha blend factor is `blend constant alpha` (see [draw_list_set_blend_constants()](#draw_list_set_blend_constants)).

- **BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR** = `11`

  Color blend factor is `1.0 - blend constant color`. Alpha blend factor is `1.0 - blend constant alpha` (see [draw_list_set_blend_constants()](#draw_list_set_blend_constants)).

- **BLEND_FACTOR_CONSTANT_ALPHA** = `12`

  Color and alpha blend factor is `blend constant alpha` (see [draw_list_set_blend_constants()](#draw_list_set_blend_constants)).

- **BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA** = `13`

  Color and alpha blend factor is `1.0 - blend constant alpha` (see [draw_list_set_blend_constants()](#draw_list_set_blend_constants)).

- **BLEND_FACTOR_SRC_ALPHA_SATURATE** = `14`

  Color blend factor is `min(source alpha, 1.0 - destination alpha)`. Alpha blend factor is `1.0`.

- **BLEND_FACTOR_SRC1_COLOR** = `15`

  Color blend factor is `second source color`. Alpha blend factor is `second source alpha`. Only relevant for dual-source blending.

- **BLEND_FACTOR_ONE_MINUS_SRC1_COLOR** = `16`

  Color blend factor is `1.0 - second source color`. Alpha blend factor is `1.0 - second source alpha`. Only relevant for dual-source blending.

- **BLEND_FACTOR_SRC1_ALPHA** = `17`

  Color and alpha blend factor is `second source alpha`. Only relevant for dual-source blending.

- **BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA** = `18`

  Color and alpha blend factor is `1.0 - second source alpha`. Only relevant for dual-source blending.

- **BLEND_FACTOR_MAX** = `19`

  Represents the size of the [BlendFactor](#enum-blendfactor) enum.

### enum BlendOperation

- **BLEND_OP_ADD** = `0`

  Additive blending operation (`source + destination`).

- **BLEND_OP_SUBTRACT** = `1`

  Subtractive blending operation (`source - destination`).

- **BLEND_OP_REVERSE_SUBTRACT** = `2`

  Reverse subtractive blending operation (`destination - source`).

- **BLEND_OP_MINIMUM** = `3`

  Minimum blending operation (keep the lowest value of the two).

- **BLEND_OP_MAXIMUM** = `4`

  Maximum blending operation (keep the highest value of the two).

- **BLEND_OP_MAX** = `5`

  Represents the size of the [BlendOperation](#enum-blendoperation) enum.

### flags PipelineDynamicStateFlags

- **DYNAMIC_STATE_LINE_WIDTH** = `1`

  Allows dynamically changing the width of rendering lines.

- **DYNAMIC_STATE_DEPTH_BIAS** = `2`

  Allows dynamically changing the depth bias.

- **DYNAMIC_STATE_BLEND_CONSTANTS** = `4`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DYNAMIC_STATE_DEPTH_BOUNDS** = `8`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DYNAMIC_STATE_STENCIL_COMPARE_MASK** = `16`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DYNAMIC_STATE_STENCIL_WRITE_MASK** = `32`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

- **DYNAMIC_STATE_STENCIL_REFERENCE** = `64`

  There is currently no description for this enum. Please help us by [contributing one](https://contributing.godotengine.org/en/latest/documentation/class_reference.html)!

### enum InitialAction

- **INITIAL_ACTION_LOAD** = `0`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

  Load the previous contents of the framebuffer.

- **INITIAL_ACTION_CLEAR** = `1`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

  Clear the whole framebuffer or its specified region.

- **INITIAL_ACTION_DISCARD** = `2`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

  Ignore the previous contents of the framebuffer. This is the fastest option if you'll overwrite all of the pixels and don't need to read any of them.

- **INITIAL_ACTION_MAX** = `3`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

  Represents the size of the [InitialAction](#enum-initialaction) enum.

- **INITIAL_ACTION_CLEAR_REGION** = `1`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

- **INITIAL_ACTION_CLEAR_REGION_CONTINUE** = `1`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

- **INITIAL_ACTION_KEEP** = `0`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

- **INITIAL_ACTION_DROP** = `2`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

- **INITIAL_ACTION_CONTINUE** = `0`

  **Deprecated:** Initial actions are solved automatically by RenderingDevice.

### enum FinalAction

- **FINAL_ACTION_STORE** = `0`

  **Deprecated:** Final actions are solved automatically by RenderingDevice.

  Store the result of the draw list in the framebuffer. This is generally what you want to do.

- **FINAL_ACTION_DISCARD** = `1`

  **Deprecated:** Final actions are solved automatically by RenderingDevice.

  Discard the contents of the framebuffer. This is the fastest option if you don't need to use the results of the draw list.

- **FINAL_ACTION_MAX** = `2`

  **Deprecated:** Final actions are solved automatically by RenderingDevice.

  Represents the size of the [FinalAction](#enum-finalaction) enum.

- **FINAL_ACTION_READ** = `0`

  **Deprecated:** Final actions are solved automatically by RenderingDevice.

- **FINAL_ACTION_CONTINUE** = `0`

  **Deprecated:** Final actions are solved automatically by RenderingDevice.

### enum ShaderStage

- **SHADER_STAGE_VERTEX** = `0`

  Vertex shader stage. This can be used to manipulate vertices from a shader (but not create new vertices).

- **SHADER_STAGE_FRAGMENT** = `1`

  Fragment shader stage (called "pixel shader" in Direct3D). This can be used to manipulate pixels from a shader.

- **SHADER_STAGE_TESSELATION_CONTROL** = `2`

  Tessellation control shader stage. This can be used to create additional geometry from a shader.

- **SHADER_STAGE_TESSELATION_EVALUATION** = `3`

  Tessellation evaluation shader stage. This can be used to create additional geometry from a shader.

- **SHADER_STAGE_COMPUTE** = `4`

  Compute shader stage. This can be used to run arbitrary computing tasks in a shader, performing them on the GPU instead of the CPU.

- **SHADER_STAGE_RAYGEN** = `5`

  Ray generation shader stage. This can be used to generate primary rays.

- **SHADER_STAGE_ANY_HIT** = `6`

  Any hit shader stage. Invoked when ray intersections are not opaque. This can be used to specify what happens when a ray hits any of the geometry in the scene.

- **SHADER_STAGE_CLOSEST_HIT** = `7`

  Closest hit shader stage. This can be used to specify what happens when a ray hits the closest geometry in the scene.

- **SHADER_STAGE_MISS** = `8`

  Miss shader stage. This can be used to specify what happens if a ray does not hit anything in the scene.

- **SHADER_STAGE_INTERSECTION** = `9`

  Intersection shader stage. The intersection shader for triangles is built-in. This can be used to compute ray intersections with primitives that are not triangles.

- **SHADER_STAGE_MAX** = `10`

  Represents the size of the [ShaderStage](#enum-shaderstage) enum.

- **SHADER_STAGE_VERTEX_BIT** = `1`

  Vertex shader stage bit (see also [SHADER_STAGE_VERTEX](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_vertex)).

- **SHADER_STAGE_FRAGMENT_BIT** = `2`

  Fragment shader stage bit (see also [SHADER_STAGE_FRAGMENT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_fragment)).

- **SHADER_STAGE_TESSELATION_CONTROL_BIT** = `4`

  Tessellation control shader stage bit (see also [SHADER_STAGE_TESSELATION_CONTROL](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_tesselation_control)).

- **SHADER_STAGE_TESSELATION_EVALUATION_BIT** = `8`

  Tessellation evaluation shader stage bit (see also [SHADER_STAGE_TESSELATION_EVALUATION](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_tesselation_evaluation)).

- **SHADER_STAGE_COMPUTE_BIT** = `16`

  Compute shader stage bit (see also [SHADER_STAGE_COMPUTE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_compute)).

- **SHADER_STAGE_RAYGEN_BIT** = `32`

  Ray generation shader stage bit (see also [SHADER_STAGE_RAYGEN](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_raygen)).

- **SHADER_STAGE_ANY_HIT_BIT** = `64`

  Any hit shader stage bit (see also [SHADER_STAGE_ANY_HIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_any_hit)).

- **SHADER_STAGE_CLOSEST_HIT_BIT** = `128`

  Closest hit shader stage bit (see also [SHADER_STAGE_CLOSEST_HIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_closest_hit)).

- **SHADER_STAGE_MISS_BIT** = `256`

  Miss shader stage bit (see also [SHADER_STAGE_MISS](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_miss)).

- **SHADER_STAGE_INTERSECTION_BIT** = `512`

  Intersection shader stage bit (see also [SHADER_STAGE_INTERSECTION](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_shader_stage_intersection)).

### enum ShaderLanguage

- **SHADER_LANGUAGE_GLSL** = `0`

  Khronos' GLSL shading language (used natively by OpenGL and Vulkan). This is the language used for core Godot shaders.

- **SHADER_LANGUAGE_HLSL** = `1`

  Microsoft's High-Level Shading Language (used natively by Direct3D, but can also be used in Vulkan).

### enum PipelineSpecializationConstantType

- **PIPELINE_SPECIALIZATION_CONSTANT_TYPE_BOOL** = `0`

  Boolean specialization constant.

- **PIPELINE_SPECIALIZATION_CONSTANT_TYPE_INT** = `1`

  Integer specialization constant.

- **PIPELINE_SPECIALIZATION_CONSTANT_TYPE_FLOAT** = `2`

  Floating-point specialization constant.

### enum Features

- **SUPPORTS_METALFX_SPATIAL** = `3`

  Support for MetalFX spatial upscaling.

- **SUPPORTS_METALFX_TEMPORAL** = `4`

  Support for MetalFX temporal upscaling.

- **SUPPORTS_BUFFER_DEVICE_ADDRESS** = `6`

  Features support for buffer device address extension.

- **SUPPORTS_IMAGE_ATOMIC_32_BIT** = `7`

  Support for 32-bit image atomic operations.

- **SUPPORTS_RAY_QUERY** = `11`

  Support for ray query extension.

  **Note:** This is currently only supported when using Vulkan. This is not supported on macOS and iOS (even on hardware supporting raytracing) due to MoltenVK limitations.

- **SUPPORTS_RAYTRACING_PIPELINE** = `12`

  Support for raytracing pipeline extension.

  **Note:** This is currently only supported when using Vulkan. This is not supported on macOS and iOS (even on hardware supporting raytracing) due to MoltenVK limitations.

- **SUPPORTS_HDR_OUTPUT** = `13`

  Support for high dynamic range (HDR) output.

### enum Limit

- **LIMIT_MAX_BOUND_UNIFORM_SETS** = `0`

  Maximum number of uniform sets that can be bound at a given time.

- **LIMIT_MAX_FRAMEBUFFER_COLOR_ATTACHMENTS** = `1`

  Maximum number of color framebuffer attachments that can be used at a given time.

- **LIMIT_MAX_TEXTURES_PER_UNIFORM_SET** = `2`

  Maximum number of textures that can be used per uniform set.

- **LIMIT_MAX_SAMPLERS_PER_UNIFORM_SET** = `3`

  Maximum number of samplers that can be used per uniform set.

- **LIMIT_MAX_STORAGE_BUFFERS_PER_UNIFORM_SET** = `4`

  Maximum number of [storage buffers](https://vkguide.dev/docs/chapter-4/storage_buffers/) per uniform set.

- **LIMIT_MAX_STORAGE_IMAGES_PER_UNIFORM_SET** = `5`

  Maximum number of storage images per uniform set.

- **LIMIT_MAX_UNIFORM_BUFFERS_PER_UNIFORM_SET** = `6`

  Maximum number of uniform buffers per uniform set.

- **LIMIT_MAX_DRAW_INDEXED_INDEX** = `7`

  Maximum index for an indexed draw command.

- **LIMIT_MAX_FRAMEBUFFER_HEIGHT** = `8`

  Maximum height of a framebuffer (in pixels).

- **LIMIT_MAX_FRAMEBUFFER_WIDTH** = `9`

  Maximum width of a framebuffer (in pixels).

- **LIMIT_MAX_TEXTURE_ARRAY_LAYERS** = `10`

  Maximum number of texture array layers.

- **LIMIT_MAX_TEXTURE_SIZE_1D** = `11`

  Maximum supported 1-dimensional texture size (in pixels on a single axis).

- **LIMIT_MAX_TEXTURE_SIZE_2D** = `12`

  Maximum supported 2-dimensional texture size (in pixels on a single axis).

- **LIMIT_MAX_TEXTURE_SIZE_3D** = `13`

  Maximum supported 3-dimensional texture size (in pixels on a single axis).

- **LIMIT_MAX_TEXTURE_SIZE_CUBE** = `14`

  Maximum supported cubemap texture size (in pixels on a single axis of a single face).

- **LIMIT_MAX_TEXTURES_PER_SHADER_STAGE** = `15`

  Maximum number of textures per shader stage.

- **LIMIT_MAX_SAMPLERS_PER_SHADER_STAGE** = `16`

  Maximum number of samplers per shader stage.

- **LIMIT_MAX_STORAGE_BUFFERS_PER_SHADER_STAGE** = `17`

  Maximum number of [storage buffers](https://vkguide.dev/docs/chapter-4/storage_buffers/) per shader stage.

- **LIMIT_MAX_STORAGE_IMAGES_PER_SHADER_STAGE** = `18`

  Maximum number of storage images per shader stage.

- **LIMIT_MAX_UNIFORM_BUFFERS_PER_SHADER_STAGE** = `19`

  Maximum number of uniform buffers per uniform set.

- **LIMIT_MAX_PUSH_CONSTANT_SIZE** = `20`

  Maximum size of a push constant. A lot of devices are limited to 128 bytes, so try to avoid exceeding 128 bytes in push constants to ensure compatibility even if your GPU is reporting a higher value.

- **LIMIT_MAX_UNIFORM_BUFFER_SIZE** = `21`

  Maximum size of a uniform buffer.

- **LIMIT_MAX_VERTEX_INPUT_ATTRIBUTE_OFFSET** = `22`

  Maximum vertex input attribute offset.

- **LIMIT_MAX_VERTEX_INPUT_ATTRIBUTES** = `23`

  Maximum number of vertex input attributes.

- **LIMIT_MAX_VERTEX_INPUT_BINDINGS** = `24`

  Maximum number of vertex input bindings.

- **LIMIT_MAX_VERTEX_INPUT_BINDING_STRIDE** = `25`

  Maximum vertex input binding stride.

- **LIMIT_MIN_UNIFORM_BUFFER_OFFSET_ALIGNMENT** = `26`

  Minimum uniform buffer offset alignment.

- **LIMIT_MAX_COMPUTE_SHARED_MEMORY_SIZE** = `27`

  Maximum shared memory size for compute shaders.

- **LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_X** = `28`

  Maximum number of workgroups for compute shaders on the X axis.

- **LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Y** = `29`

  Maximum number of workgroups for compute shaders on the Y axis.

- **LIMIT_MAX_COMPUTE_WORKGROUP_COUNT_Z** = `30`

  Maximum number of workgroups for compute shaders on the Z axis.

- **LIMIT_MAX_COMPUTE_WORKGROUP_INVOCATIONS** = `31`

  Maximum number of workgroup invocations for compute shaders.

- **LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_X** = `32`

  Maximum workgroup size for compute shaders on the X axis.

- **LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Y** = `33`

  Maximum workgroup size for compute shaders on the Y axis.

- **LIMIT_MAX_COMPUTE_WORKGROUP_SIZE_Z** = `34`

  Maximum workgroup size for compute shaders on the Z axis.

- **LIMIT_MAX_VIEWPORT_DIMENSIONS_X** = `35`

  Maximum viewport width (in pixels).

- **LIMIT_MAX_VIEWPORT_DIMENSIONS_Y** = `36`

  Maximum viewport height (in pixels).

- **LIMIT_METALFX_TEMPORAL_SCALER_MIN_SCALE** = `46`

  Returns the smallest value for [ProjectSettings.rendering/scaling_3d/scale](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/scaling_3d/scale) when using the MetalFX temporal upscaler.

  **Note:** The returned value is multiplied by a factor of `1000000` to preserve 6 digits of precision. It must be divided by `1000000.0` to convert the value to a floating point number.

- **LIMIT_METALFX_TEMPORAL_SCALER_MAX_SCALE** = `47`

  Returns the largest value for [ProjectSettings.rendering/scaling_3d/scale](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/scaling_3d/scale) when using the MetalFX temporal upscaler.

  **Note:** The returned value is multiplied by a factor of `1000000` to preserve 6 digits of precision. It must be divided by `1000000.0` to convert the value to a floating point number.

### enum MemoryType

- **MEMORY_TEXTURES** = `0`

  Memory taken by textures.

- **MEMORY_BUFFERS** = `1`

  Memory taken by buffers.

- **MEMORY_TOTAL** = `2`

  Total memory taken. This is greater than the sum of [MEMORY_TEXTURES](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_memory_textures) and [MEMORY_BUFFERS](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_memory_buffers), as it also includes miscellaneous memory usage.

### enum BreadcrumbMarker

- **NONE** = `0`

  No breadcrumb marker will be added.

- **REFLECTION_PROBES** = `65536`

  During a GPU crash in dev or debug mode, Godot's error message will include `"REFLECTION_PROBES"` for added context as to when the crash occurred.

- **SKY_PASS** = `131072`

  During a GPU crash in dev or debug mode, Godot's error message will include `"SKY_PASS"` for added context as to when the crash occurred.

- **LIGHTMAPPER_PASS** = `196608`

  During a GPU crash in dev or debug mode, Godot's error message will include `"LIGHTMAPPER_PASS"` for added context as to when the crash occurred.

- **SHADOW_PASS_DIRECTIONAL** = `262144`

  During a GPU crash in dev or debug mode, Godot's error message will include `"SHADOW_PASS_DIRECTIONAL"` for added context as to when the crash occurred.

- **SHADOW_PASS_CUBE** = `327680`

  During a GPU crash in dev or debug mode, Godot's error message will include `"SHADOW_PASS_CUBE"` for added context as to when the crash occurred.

- **OPAQUE_PASS** = `393216`

  During a GPU crash in dev or debug mode, Godot's error message will include `"OPAQUE_PASS"` for added context as to when the crash occurred.

- **ALPHA_PASS** = `458752`

  During a GPU crash in dev or debug mode, Godot's error message will include `"ALPHA_PASS"` for added context as to when the crash occurred.

- **TRANSPARENT_PASS** = `524288`

  During a GPU crash in dev or debug mode, Godot's error message will include `"TRANSPARENT_PASS"` for added context as to when the crash occurred.

- **POST_PROCESSING_PASS** = `589824`

  During a GPU crash in dev or debug mode, Godot's error message will include `"POST_PROCESSING_PASS"` for added context as to when the crash occurred.

- **BLIT_PASS** = `655360`

  During a GPU crash in dev or debug mode, Godot's error message will include `"BLIT_PASS"` for added context as to when the crash occurred.

- **UI_PASS** = `720896`

  During a GPU crash in dev or debug mode, Godot's error message will include `"UI_PASS"` for added context as to when the crash occurred.

- **DEBUG_PASS** = `786432`

  During a GPU crash in dev or debug mode, Godot's error message will include `"DEBUG_PASS"` for added context as to when the crash occurred.

### flags DrawFlags

- **DRAW_DEFAULT_ALL** = `0`

  Do not clear or ignore any attachments.

- **DRAW_CLEAR_COLOR_0** = `1`

  Clear the first color attachment.

- **DRAW_CLEAR_COLOR_1** = `2`

  Clear the second color attachment.

- **DRAW_CLEAR_COLOR_2** = `4`

  Clear the third color attachment.

- **DRAW_CLEAR_COLOR_3** = `8`

  Clear the fourth color attachment.

- **DRAW_CLEAR_COLOR_4** = `16`

  Clear the fifth color attachment.

- **DRAW_CLEAR_COLOR_5** = `32`

  Clear the sixth color attachment.

- **DRAW_CLEAR_COLOR_6** = `64`

  Clear the seventh color attachment.

- **DRAW_CLEAR_COLOR_7** = `128`

  Clear the eighth color attachment.

- **DRAW_CLEAR_COLOR_MASK** = `255`

  Mask for clearing all color attachments.

- **DRAW_CLEAR_COLOR_ALL** = `255`

  Clear all color attachments.

- **DRAW_IGNORE_COLOR_0** = `256`

  Ignore the previous contents of the first color attachment.

- **DRAW_IGNORE_COLOR_1** = `512`

  Ignore the previous contents of the second color attachment.

- **DRAW_IGNORE_COLOR_2** = `1024`

  Ignore the previous contents of the third color attachment.

- **DRAW_IGNORE_COLOR_3** = `2048`

  Ignore the previous contents of the fourth color attachment.

- **DRAW_IGNORE_COLOR_4** = `4096`

  Ignore the previous contents of the fifth color attachment.

- **DRAW_IGNORE_COLOR_5** = `8192`

  Ignore the previous contents of the sixth color attachment.

- **DRAW_IGNORE_COLOR_6** = `16384`

  Ignore the previous contents of the seventh color attachment.

- **DRAW_IGNORE_COLOR_7** = `32768`

  Ignore the previous contents of the eighth color attachment.

- **DRAW_IGNORE_COLOR_MASK** = `65280`

  Mask for ignoring all the previous contents of the color attachments.

- **DRAW_IGNORE_COLOR_ALL** = `65280`

  Ignore the previous contents of all color attachments.

- **DRAW_CLEAR_DEPTH** = `65536`

  Clear the depth attachment.

- **DRAW_IGNORE_DEPTH** = `131072`

  Ignore the previous contents of the depth attachment.

- **DRAW_CLEAR_STENCIL** = `262144`

  Clear the stencil attachment.

- **DRAW_IGNORE_STENCIL** = `524288`

  Ignore the previous contents of the stencil attachment.

- **DRAW_CLEAR_ALL** = `327935`

  Clear all attachments.

- **DRAW_IGNORE_ALL** = `720640`

  Ignore the previous contents of all attachments.

## Constants

- **INVALID_ID** = `-1`

  Returned by functions that return an ID if a value is invalid.

- **INVALID_FORMAT_ID** = `-1`

  Returned by functions that return a format ID if a value is invalid.

## Method Descriptions

### barrier

`void barrier(from: bitfield[BarrierMask] = 32767, to: bitfield[BarrierMask] = 32767)`

**Deprecated:** Barriers are automatically inserted by RenderingDevice.

This method does nothing.

### blas_build

`Error blas_build(blas: RID)`

**Experimental:** This method may be changed or removed in future versions.

Builds the `blas`.

### blas_create

`RID blas_create(geometries: Array[RDAccelerationStructureGeometry], flags: bitfield[AccelerationStructureFlagBits])`

**Experimental:** This method may be changed or removed in future versions.

Creates a new Bottom-Level Acceleration Structure (BLAS). It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### buffer_clear

`Error buffer_clear(buffer: RID, offset: int, size_bytes: int)`

Clears the contents of the `buffer`, clearing `size_bytes` bytes, starting at `offset`.

Prints an error if:

- the size isn't a multiple of four

- the region specified by `offset` + `size_bytes` exceeds the buffer

- a draw list is currently active (created by [draw_list_begin()](#draw_list_begin))

- a compute list is currently active (created by [compute_list_begin()](#compute_list_begin))

### buffer_copy

`Error buffer_copy(src_buffer: RID, dst_buffer: RID, src_offset: int, dst_offset: int, size: int)`

Copies `size` bytes from the `src_buffer` at `src_offset` into `dst_buffer` at `dst_offset`.

Prints an error if:

- `size` exceeds the size of either `src_buffer` or `dst_buffer` at their corresponding offsets

- a draw list is currently active (created by [draw_list_begin()](#draw_list_begin))

- a compute list is currently active (created by [compute_list_begin()](#compute_list_begin))

### buffer_get_data

`PackedByteArray buffer_get_data(buffer: RID, offset_bytes: int = 0, size_bytes: int = 0)`

Returns a copy of the data of the specified `buffer`, optionally `offset_bytes` and `size_bytes` can be set to copy only a portion of the buffer.

**Note:** This method will block the GPU from working until the data is retrieved. Refer to [buffer_get_data_async()](#buffer_get_data_async) for an alternative that returns the data in more performant way.

### buffer_get_data_async

`Error buffer_get_data_async(buffer: RID, callback: Callable, offset_bytes: int = 0, size_bytes: int = 0)`

Asynchronous version of [buffer_get_data()](#buffer_get_data). RenderingDevice will call `callback` in a certain amount of frames with the data the buffer had at the time of the request.

**Note:** At the moment, the delay corresponds to the amount of frames specified by [ProjectSettings.rendering/rendering_device/vsync/frame_queue_size](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/rendering_device/vsync/frame_queue_size).

**Note:** Downloading large buffers can have a prohibitive cost for real-time even when using the asynchronous method due to hardware bandwidth limitations. When dealing with large resources, you can adjust settings such as [ProjectSettings.rendering/rendering_device/staging_buffer/block_size_kb](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/rendering_device/staging_buffer/block_size_kb) to improve the transfer speed at the cost of extra memory.

```
func _buffer_get_data_callback(array):
    value = array.decode_u32(0)

...

rd.buffer_get_data_async(buffer, _buffer_get_data_callback)
```

### buffer_get_device_address

`int buffer_get_device_address(buffer: RID)`

Returns the address of the given `buffer` which can be passed to shaders in any way to access underlying data. Buffer must have been created with this feature enabled.

**Note:** You must check that the GPU supports this functionality by calling [has_feature()](#has_feature) with [SUPPORTS_BUFFER_DEVICE_ADDRESS](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_supports_buffer_device_address) as a parameter.

### buffer_update

`Error buffer_update(buffer: RID, offset: int, size_bytes: int, data: PackedByteArray)`

Updates a region of `size_bytes` bytes, starting at `offset`, in the buffer, with the specified `data`.

Prints an error if:

- the region specified by `offset` + `size_bytes` exceeds the buffer

- a draw list is currently active (created by [draw_list_begin()](#draw_list_begin))

- a compute list is currently active (created by [compute_list_begin()](#compute_list_begin))

### capture_timestamp

`void capture_timestamp(name: String)`

Creates a timestamp marker with the specified `name`. This is used for performance reporting with the [get_captured_timestamp_cpu_time()](#get_captured_timestamp_cpu_time), [get_captured_timestamp_gpu_time()](#get_captured_timestamp_gpu_time) and [get_captured_timestamp_name()](#get_captured_timestamp_name) methods.

### compute_list_add_barrier

`void compute_list_add_barrier(compute_list: int)`

Raises a Vulkan compute barrier in the specified `compute_list`.

### compute_list_begin

`int compute_list_begin()`

Starts a list of compute commands created with the `compute_*` methods. The returned value should be passed to other `compute_list_*` functions.

Multiple compute lists cannot be created at the same time; you must finish the previous compute list first using [compute_list_end()](#compute_list_end).

A simple compute operation might look like this (code is not a complete example):

```
var rd = RenderingDevice.new()
var compute_list = rd.compute_list_begin()

rd.compute_list_bind_compute_pipeline(compute_list, compute_shader_dilate_pipeline)
rd.compute_list_bind_uniform_set(compute_list, compute_base_uniform_set, 0)
rd.compute_list_bind_uniform_set(compute_list, dilate_uniform_set, 1)

for i in atlas_slices:
    rd.compute_list_set_push_constant(compute_list, push_constant, push_constant.size())
    rd.compute_list_dispatch(compute_list, group_size.x, group_size.y, group_size.z)
    # No barrier, let them run all together.

rd.compute_list_end()
```

### compute_list_bind_compute_pipeline

`void compute_list_bind_compute_pipeline(compute_list: int, compute_pipeline: RID)`

Tells the GPU what compute pipeline to use when processing the compute list. If the shader has changed since the last time this function was called, Godot will unbind all descriptor sets and will re-bind them inside [compute_list_dispatch()](#compute_list_dispatch).

### compute_list_bind_uniform_set

`void compute_list_bind_uniform_set(compute_list: int, uniform_set: RID, set_index: int)`

Binds the `uniform_set` to this `compute_list`. Godot ensures that all textures in the uniform set have the correct Vulkan access masks. If Godot had to change access masks of textures, it will raise a Vulkan image memory barrier.

### compute_list_dispatch

`void compute_list_dispatch(compute_list: int, x_groups: int, y_groups: int, z_groups: int)`

Submits the compute list for processing on the GPU. This is the compute equivalent to [draw_list_draw()](#draw_list_draw).

### compute_list_dispatch_indirect

`void compute_list_dispatch_indirect(compute_list: int, buffer: RID, offset: int)`

Submits the compute list for processing on the GPU with the given group counts stored in the `buffer` at `offset`. Buffer must have been created with [STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_storage_buffer_usage_dispatch_indirect) flag.

### compute_list_end

`void compute_list_end()`

Finishes a list of compute commands created with the `compute_*` methods.

### compute_list_set_push_constant

`void compute_list_set_push_constant(compute_list: int, buffer: PackedByteArray, size_bytes: int)`

Sets the push constant data to `buffer` for the specified `compute_list`. The shader determines how this binary data is used. The buffer's size in bytes must also be specified in `size_bytes` (this can be obtained by calling the [PackedByteArray.size()](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html#class_packedbytearray_method_size) method on the passed `buffer`).

### compute_pipeline_create

`RID compute_pipeline_create(shader: RID, specialization_constants: Array[RDPipelineSpecializationConstant] = [])`

Creates a new compute pipeline. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when the `shader` is freed.

### compute_pipeline_is_valid

`bool compute_pipeline_is_valid(compute_pipeline: RID)`

Returns `true` if the compute pipeline specified by the `compute_pipeline` RID is valid, `false` otherwise.

### create_local_device

`RenderingDevice create_local_device()`

Create a new local **RenderingDevice**. This is most useful for performing compute operations on the GPU independently from the rest of the engine.

### draw_command_begin_label

`void draw_command_begin_label(name: String, color: Color)`

Create a command buffer debug label region that can be displayed in third-party tools such as [RenderDoc](https://renderdoc.org/). All regions must be ended with a [draw_command_end_label()](#draw_command_end_label) call. When viewed from the linear series of submissions to a single queue, calls to [draw_command_begin_label()](#draw_command_begin_label) and [draw_command_end_label()](#draw_command_end_label) must be matched and balanced.

The `VK_EXT_DEBUG_UTILS_EXTENSION_NAME` Vulkan extension must be available and enabled for command buffer debug label region to work. See also [draw_command_end_label()](#draw_command_end_label).

### draw_command_end_label

`void draw_command_end_label()`

Ends the command buffer debug label region started by a [draw_command_begin_label()](#draw_command_begin_label) call.

### draw_command_insert_label

`void draw_command_insert_label(name: String, color: Color)`

**Deprecated:** Inserting labels no longer applies due to command reordering.

This method does nothing.

### draw_list_begin

`int draw_list_begin(framebuffer: RID, draw_flags: bitfield[DrawFlags] = 0, clear_color_values: PackedColorArray = PackedColorArray(), clear_depth_value: float = 1.0, clear_stencil_value: int = 0, region: Rect2 = Rect2(0, 0, 0, 0), breadcrumb: int = 0)`

Starts a list of raster drawing commands created with the `draw_*` methods. The returned value should be passed to other `draw_list_*` functions.

Multiple draw lists cannot be created at the same time; you must finish the previous draw list first using [draw_list_end()](#draw_list_end).

A simple drawing operation might look like this (code is not a complete example):

```
var rd = RenderingDevice.new()
var clear_colors = PackedColorArray([Color(0, 0, 0, 0), Color(0, 0, 0, 0), Color(0, 0, 0, 0)])
var draw_list = rd.draw_list_begin(framebuffers[i], RenderingDevice.CLEAR_COLOR_ALL, clear_colors, true, 1.0f, true, 0, Rect2(), RenderingDevice.OPAQUE_PASS)

# Draw opaque.
rd.draw_list_bind_render_pipeline(draw_list, raster_pipeline)
rd.draw_list_bind_uniform_set(draw_list, raster_base_uniform, 0)
rd.draw_list_set_push_constant(draw_list, raster_push_constant, raster_push_constant.size())
rd.draw_list_draw(draw_list, false, 1, slice_triangle_count[i] * 3)
# Draw wire.
rd.draw_list_bind_render_pipeline(draw_list, raster_pipeline_wire)
rd.draw_list_bind_uniform_set(draw_list, raster_base_uniform, 0)
rd.draw_list_set_push_constant(draw_list, raster_push_constant, raster_push_constant.size())
rd.draw_list_draw(draw_list, false, 1, slice_triangle_count[i] * 3)

rd.draw_list_end()
```

The `draw_flags` indicates if the texture attachments of the framebuffer should be cleared or ignored. Only one of the two flags can be used for each individual attachment. Ignoring an attachment means that any contents that existed before the draw list will be completely discarded, reducing the memory bandwidth used by the render pass but producing garbage results if the pixels aren't replaced. The default behavior allows the engine to figure out the right operation to use if the texture is discardable, which can result in increased performance. See [RDTextureFormat](https://docs.godotengine.org/en/stable/classes/class_rdtextureformat.html) or [texture_set_discardable()](#texture_set_discardable).

The `breadcrumb` parameter can be an arbitrary 32-bit integer that is useful to diagnose GPU crashes. If Godot is built in dev or debug mode; when the GPU crashes Godot will dump all shaders that were being executed at the time of the crash and the breadcrumb is useful to diagnose what passes did those shaders belong to.

It does not affect rendering behavior and can be set to 0. It is recommended to use [BreadcrumbMarker](#enum-breadcrumbmarker) enumerations for consistency but it's not required. It is also possible to use bitwise operations to add extra data. e.g.

```
rd.draw_list_begin(fb[i], RenderingDevice.CLEAR_COLOR_ALL, clear_colors, true, 1.0f, true, 0, Rect2(), RenderingDevice.OPAQUE_PASS | 5)
```

### draw_list_begin_for_screen

`int draw_list_begin_for_screen(screen: int = 0, clear_color: Color = Color(0, 0, 0, 1))`

High-level variant of [draw_list_begin()](#draw_list_begin), with the parameters automatically being adjusted for drawing onto the window specified by the `screen` ID.

**Note:** Cannot be used with local RenderingDevices, as these don't have a screen. If called on a local RenderingDevice, [draw_list_begin_for_screen()](#draw_list_begin_for_screen) returns [INVALID_ID](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_invalid_id).

### draw_list_begin_split

`PackedInt64Array draw_list_begin_split(framebuffer: RID, splits: int, initial_color_action: InitialAction, final_color_action: FinalAction, initial_depth_action: InitialAction, final_depth_action: FinalAction, clear_color_values: PackedColorArray = PackedColorArray(), clear_depth: float = 1.0, clear_stencil: int = 0, region: Rect2 = Rect2(0, 0, 0, 0), storage_textures: Array[RID] = [])`

**Deprecated:** Split draw lists are used automatically by RenderingDevice.

This method does nothing and always returns an empty [PackedInt64Array](https://docs.godotengine.org/en/stable/classes/class_packedint64array.html).

### draw_list_bind_index_array

`void draw_list_bind_index_array(draw_list: int, index_array: RID)`

Binds `index_array` to the specified `draw_list`.

### draw_list_bind_render_pipeline

`void draw_list_bind_render_pipeline(draw_list: int, render_pipeline: RID)`

Binds `render_pipeline` to the specified `draw_list`.

### draw_list_bind_uniform_set

`void draw_list_bind_uniform_set(draw_list: int, uniform_set: RID, set_index: int)`

Binds `uniform_set` to the specified `draw_list`. A `set_index` must also be specified, which is an identifier starting from `0` that must match the one expected by the draw list.

### draw_list_bind_vertex_array

`void draw_list_bind_vertex_array(draw_list: int, vertex_array: RID)`

Binds `vertex_array` to the specified `draw_list`.

### draw_list_bind_vertex_buffers_format

`void draw_list_bind_vertex_buffers_format(draw_list: int, vertex_format: int, vertex_count: int, vertex_buffers: Array[RID], offsets: PackedInt64Array = PackedInt64Array())`

Binds a set of `vertex_buffers` directly to the specified `draw_list` using `vertex_format` without creating a vertex array RID. Provide the number of vertices in `vertex_count`; optional per-buffer byte `offsets` may also be supplied.

### draw_list_disable_scissor

`void draw_list_disable_scissor(draw_list: int)`

Removes and disables the scissor rectangle for the specified `draw_list`. See also [draw_list_enable_scissor()](#draw_list_enable_scissor).

### draw_list_draw

`void draw_list_draw(draw_list: int, use_indices: bool, instances: int, procedural_vertex_count: int = 0)`

Submits `draw_list` for rendering on the GPU. This is the raster equivalent to [compute_list_dispatch()](#compute_list_dispatch).

### draw_list_draw_indirect

`void draw_list_draw_indirect(draw_list: int, use_indices: bool, buffer: RID, offset: int = 0, draw_count: int = 1, stride: int = 0)`

Submits `draw_list` for rendering on the GPU with the given parameters stored in the `buffer` at `offset`. Parameters being integers: vertex count, instance count, first vertex, first instance. And when using indices: index count, instance count, first index, vertex offset, first instance. Buffer must have been created with [STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_storage_buffer_usage_dispatch_indirect) flag.

### draw_list_enable_scissor

`void draw_list_enable_scissor(draw_list: int, rect: Rect2 = Rect2(0, 0, 0, 0))`

Creates a scissor rectangle and enables it for the specified `draw_list`. Scissor rectangles are used for clipping by discarding fragments that fall outside a specified rectangular portion of the screen. See also [draw_list_disable_scissor()](#draw_list_disable_scissor).

**Note:** The specified `rect` is automatically intersected with the screen's dimensions, which means it cannot exceed the screen's dimensions.

### draw_list_end

`void draw_list_end()`

Finishes a list of raster drawing commands created with the `draw_*` methods.

### draw_list_set_blend_constants

`void draw_list_set_blend_constants(draw_list: int, color: Color)`

Sets blend constants for the specified `draw_list` to `color`. Blend constants are used only if the graphics pipeline is created with [DYNAMIC_STATE_BLEND_CONSTANTS](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_dynamic_state_blend_constants) flag set.

### draw_list_set_push_constant

`void draw_list_set_push_constant(draw_list: int, buffer: PackedByteArray, size_bytes: int)`

Sets the push constant data to `buffer` for the specified `draw_list`. The shader determines how this binary data is used. The buffer's size in bytes must also be specified in `size_bytes` (this can be obtained by calling the [PackedByteArray.size()](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html#class_packedbytearray_method_size) method on the passed `buffer`).

### draw_list_switch_to_next_pass

`int draw_list_switch_to_next_pass()`

Switches to the next draw pass.

### draw_list_switch_to_next_pass_split

`PackedInt64Array draw_list_switch_to_next_pass_split(splits: int)`

**Deprecated:** Split draw lists are used automatically by RenderingDevice.

This method does nothing and always returns an empty [PackedInt64Array](https://docs.godotengine.org/en/stable/classes/class_packedint64array.html).

### framebuffer_create

`RID framebuffer_create(textures: Array[RID], validate_with_format: int = -1, view_count: int = 1)`

Creates a new framebuffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when any of the `textures` is freed.

### framebuffer_create_empty

`RID framebuffer_create_empty(size: Vector2i, samples: TextureSamples = 0, validate_with_format: int = -1)`

Creates a new empty framebuffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### framebuffer_create_multipass

`RID framebuffer_create_multipass(textures: Array[RID], passes: Array[RDFramebufferPass], validate_with_format: int = -1, view_count: int = 1)`

Creates a new multipass framebuffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when any of the `textures` is freed.

### framebuffer_format_create

`int framebuffer_format_create(attachments: Array[RDAttachmentFormat], view_count: int = 1)`

Creates a new framebuffer format with the specified `attachments` and `view_count`. Returns the new framebuffer's unique framebuffer format ID.

If `view_count` is greater than or equal to `2`, enables multiview which is used for VR rendering. This requires support for the Vulkan multiview extension.

### framebuffer_format_create_empty

`int framebuffer_format_create_empty(samples: TextureSamples = 0)`

Creates a new empty framebuffer format with the specified number of `samples` and returns its ID.

### framebuffer_format_create_multipass

`int framebuffer_format_create_multipass(attachments: Array[RDAttachmentFormat], passes: Array[RDFramebufferPass], view_count: int = 1)`

Creates a multipass framebuffer format with the specified `attachments`, `passes` and `view_count` and returns its ID. If `view_count` is greater than or equal to `2`, enables multiview which is used for VR rendering. This requires support for the Vulkan multiview extension.

### framebuffer_format_get_texture_samples

`TextureSamples framebuffer_format_get_texture_samples(format: int, render_pass: int = 0)`

Returns the number of texture samples used for the given framebuffer `format` ID (returned by [framebuffer_get_format()](#framebuffer_get_format)).

### framebuffer_get_format

`int framebuffer_get_format(framebuffer: RID)`

Returns the format ID of the framebuffer specified by the `framebuffer` RID. This ID is guaranteed to be unique for the same formats and does not need to be freed.

### framebuffer_is_valid

`bool framebuffer_is_valid(framebuffer: RID)`

Returns `true` if the framebuffer specified by the `framebuffer` RID is valid, `false` otherwise.

### free_rid

`void free_rid(rid: RID)`

Tries to free an object in the RenderingDevice. To avoid memory leaks, this should be called after using an object as memory management does not occur automatically when using RenderingDevice directly.

### full_barrier

`void full_barrier()`

**Deprecated:** Barriers are automatically inserted by RenderingDevice.

This method does nothing.

### get_captured_timestamp_cpu_time

`int get_captured_timestamp_cpu_time(index: int)`

Returns the timestamp in CPU time for the rendering step specified by `index` (in microseconds since the engine started). See also [get_captured_timestamp_gpu_time()](#get_captured_timestamp_gpu_time) and [capture_timestamp()](#capture_timestamp).

### get_captured_timestamp_gpu_time

`int get_captured_timestamp_gpu_time(index: int)`

Returns the timestamp in GPU time for the rendering step specified by `index` (in microseconds since the engine started). See also [get_captured_timestamp_cpu_time()](#get_captured_timestamp_cpu_time) and [capture_timestamp()](#capture_timestamp).

### get_captured_timestamp_name

`String get_captured_timestamp_name(index: int)`

Returns the timestamp's name for the rendering step specified by `index`. See also [capture_timestamp()](#capture_timestamp).

### get_captured_timestamps_count

`int get_captured_timestamps_count()`

Returns the total number of timestamps (rendering steps) available for profiling.

### get_captured_timestamps_frame

`int get_captured_timestamps_frame()`

Returns the index of the last frame rendered that has rendering timestamps available for querying.

### get_device_allocation_count

`int get_device_allocation_count()`

Returns how many allocations the GPU has performed for internal driver structures.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_device_allocs_by_object_type

`int get_device_allocs_by_object_type(type: int)`

Same as [get_device_allocation_count()](#get_device_allocation_count) but filtered for a given object type.

The type argument must be in range `[0; get_tracked_object_type_count - 1]`. If [get_tracked_object_type_count()](#get_tracked_object_type_count) is 0, then type argument is ignored and always returns 0.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_device_memory_by_object_type

`int get_device_memory_by_object_type(type: int)`

Same as [get_device_total_memory()](#get_device_total_memory) but filtered for a given object type.

The type argument must be in range `[0; get_tracked_object_type_count - 1]`. If [get_tracked_object_type_count()](#get_tracked_object_type_count) is 0, then type argument is ignored and always returns 0.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_device_name

`String get_device_name()`

Returns the name of the video adapter (e.g. "GeForce GTX 1080/PCIe/SSE2"). Equivalent to [RenderingServer.get_video_adapter_name()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_get_video_adapter_name). See also [get_device_vendor_name()](#get_device_vendor_name).

### get_device_pipeline_cache_uuid

`String get_device_pipeline_cache_uuid()`

Returns the universally unique identifier for the pipeline cache. This is used to cache shader files on disk, which avoids shader recompilations on subsequent engine runs. This UUID varies depending on the graphics card model, but also the driver version. Therefore, updating graphics drivers will invalidate the shader cache.

### get_device_total_memory

`int get_device_total_memory()`

Returns how much bytes the GPU is using.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_device_vendor_name

`String get_device_vendor_name()`

Returns the vendor of the video adapter (e.g. "NVIDIA Corporation"). Equivalent to [RenderingServer.get_video_adapter_vendor()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_get_video_adapter_vendor). See also [get_device_name()](#get_device_name).

### get_driver_allocation_count

`int get_driver_allocation_count()`

Returns how many allocations the GPU driver has performed for internal driver structures.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_driver_allocs_by_object_type

`int get_driver_allocs_by_object_type(type: int)`

Same as [get_driver_allocation_count()](#get_driver_allocation_count) but filtered for a given object type.

The type argument must be in range `[0; get_tracked_object_type_count - 1]`. If [get_tracked_object_type_count()](#get_tracked_object_type_count) is 0, then type argument is ignored and always returns 0.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_driver_and_device_memory_report

`String get_driver_and_device_memory_report()`

Returns string report in CSV format using the following methods:

- [get_tracked_object_name()](#get_tracked_object_name)\

- [get_tracked_object_type_count()](#get_tracked_object_type_count)\

- [get_driver_total_memory()](#get_driver_total_memory)\

- [get_driver_allocation_count()](#get_driver_allocation_count)\

- [get_driver_memory_by_object_type()](#get_driver_memory_by_object_type)\

- [get_driver_allocs_by_object_type()](#get_driver_allocs_by_object_type)\

- [get_device_total_memory()](#get_device_total_memory)\

- [get_device_allocation_count()](#get_device_allocation_count)\

- [get_device_memory_by_object_type()](#get_device_memory_by_object_type)\

- [get_device_allocs_by_object_type()](#get_device_allocs_by_object_type)\

This is only used by Vulkan in debug builds. Godot must also be started with the `--extra-gpu-memory-tracking` [command line argument](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html).

### get_driver_memory_by_object_type

`int get_driver_memory_by_object_type(type: int)`

Same as [get_driver_total_memory()](#get_driver_total_memory) but filtered for a given object type.

The type argument must be in range `[0; get_tracked_object_type_count - 1]`. If [get_tracked_object_type_count()](#get_tracked_object_type_count) is 0, then type argument is ignored and always returns 0.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_driver_resource

`int get_driver_resource(resource: DriverResource, rid: RID, index: int)`

Returns the unique identifier of the driver `resource` for the specified `rid`. Some driver resource types ignore the specified `rid`. `index` is always ignored but must be specified anyway.

### get_driver_total_memory

`int get_driver_total_memory()`

Returns how much bytes the GPU driver is using for internal driver structures.

This is only used by Vulkan in debug builds and can return 0 when this information is not tracked or unknown.

### get_frame_delay

`int get_frame_delay()`

Returns the frame count kept by the graphics API. Higher values result in higher input lag, but with more consistent throughput. For the main **RenderingDevice**, frames are cycled (usually 3 with triple-buffered V-Sync enabled). However, local **RenderingDevice** s only have 1 frame.

### get_memory_usage

`int get_memory_usage(type: MemoryType)`

Returns the memory usage in bytes corresponding to the given `type`. When using Vulkan, these statistics are calculated by [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator).

### get_perf_report

`String get_perf_report()`

Returns a string with a performance report from the past frame. Updates every frame.

### get_tracked_object_name

`String get_tracked_object_name(type_index: int)`

Returns the name of the type of object for the given `type_index`. This value must be in range `[0; get_tracked_object_type_count - 1]`. If [get_tracked_object_type_count()](#get_tracked_object_type_count) is 0, then type argument is ignored and always returns the same string.

The return value is important because it gives meaning to the types passed to [get_driver_memory_by_object_type()](#get_driver_memory_by_object_type), [get_driver_allocs_by_object_type()](#get_driver_allocs_by_object_type), [get_device_memory_by_object_type()](#get_device_memory_by_object_type), and [get_device_allocs_by_object_type()](#get_device_allocs_by_object_type). Examples of strings it can return (not exhaustive):

- DEVICE_MEMORY

- PIPELINE_CACHE

- SWAPCHAIN_KHR

- COMMAND_POOL

Thus if e.g. `get_tracked_object_name(5)` returns "COMMAND_POOL", then `get_device_memory_by_object_type(5)` returns the bytes used by the GPU for command pools.

This is only used by Vulkan in debug builds. Godot must also be started with the `--extra-gpu-memory-tracking` [command line argument](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html).

### get_tracked_object_type_count

`int get_tracked_object_type_count()`

Returns how many types of trackable objects there are.

This is only used by Vulkan in debug builds. Godot must also be started with the `--extra-gpu-memory-tracking` [command line argument](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html).

### has_feature

`bool has_feature(feature: Features)`

Returns `true` if the `feature` is supported by the GPU.

### hit_sbt_create

`RID hit_sbt_create(raytracing_pipeline: RID, initial_hit_group_capacity: int)`

**Experimental:** This method may be changed or removed in future versions.

Creates a new hit shader binding table (SBT). It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when the `raytracing_pipeline` is freed.

The hit SBT resizes itself as needed. `initial_hit_group_capacity` is used to allocate the initial backing memory.

### hit_sbt_range_alloc

`int hit_sbt_range_alloc(hit_sbt: RID, hit_group_count: int)`

**Experimental:** This method may be changed or removed in future versions.

Allocates a contiguous range of SBT entries from `hit_sbt`.

The returned value should be assigned to [RDAccelerationStructureInstance.hit_sbt_range](https://docs.godotengine.org/en/stable/classes/class_rdaccelerationstructureinstance.html#class_rdaccelerationstructureinstance_property_hit_sbt_range).

During ray traversal, hit group index is computed as:

(geometry index in [RDAccelerationStructureInstance.blas](https://docs.godotengine.org/en/stable/classes/class_rdaccelerationstructureinstance.html#class_rdaccelerationstructureinstance_property_blas))

× (SBT stride used in `traceRayEXT`)

`hit_group_count` must be large enough to cover all SBT entries that may be indexed by this equation. This typically corresponds to:

(geometry count in [RDAccelerationStructureInstance.blas](https://docs.godotengine.org/en/stable/classes/class_rdaccelerationstructureinstance.html#class_rdaccelerationstructureinstance_property_blas))

× (SBT stride used in `traceRayEXT`)

The allocated range is uninitialized and must be filled using [hit_sbt_range_update()](#hit_sbt_range_update).

### hit_sbt_range_free

`Error hit_sbt_range_free(hit_sbt: RID, range: int)`

**Experimental:** This method may be changed or removed in future versions.

Frees a hit SBT range previously allocated with [hit_sbt_range_alloc()](#hit_sbt_range_alloc).

The range must not be in use by any acceleration structure after being freed.

### hit_sbt_range_update

`Error hit_sbt_range_update(hit_sbt: RID, range: int, offset: int, hit_group_indices: PackedInt32Array)`

**Experimental:** This method may be changed or removed in future versions.

Updates the contents of a hit SBT range.

`hit_group_indices` specifies indices into the hit group array provided in [raytracing_pipeline_create()](#raytracing_pipeline_create).

The `offset` parameter specifies where within the allocated range the writing begins. This allows partial updates of a range. However, the complete range must be fully initialized before it is used in a raytracing dispatch.

### hit_sbt_set_pipeline

`Error hit_sbt_set_pipeline(hit_sbt: RID, raytracing_pipeline: RID)`

**Experimental:** This method may be changed or removed in future versions.

Sets a new `raytracing_pipeline` for `hit_sbt`.

The new pipeline must be a superset of the previous one. Existing hit groups must keep the same order and new hit groups should be appended to the end. This preserves existing SBT entries.

The previous pipeline must remain valid during the call.

### index_array_create

`RID index_array_create(index_buffer: RID, index_offset: int, index_count: int)`

Creates a new index array. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when the `index_buffer` is freed.

### index_buffer_create

`RID index_buffer_create(size_indices: int, format: IndexBufferFormat, data: PackedByteArray = PackedByteArray(), use_restart_indices: bool = false, creation_bits: bitfield[BufferCreationBits] = 0)`

Creates a new index buffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### limit_get

`int limit_get(limit: Limit)`

Returns the value of the specified `limit`. This limit varies depending on the current graphics hardware (and sometimes the driver version). If the given limit is exceeded, rendering errors will occur.

Limits for various graphics hardware can be found in the [Vulkan Hardware Database](https://vulkan.gpuinfo.org/).

### raytracing_list_begin

`int raytracing_list_begin()`

**Experimental:** This method may be changed or removed in future versions.

Starts a list of raytracing commands. The returned value should be passed to other `raytracing_list_*` functions.

Multiple raytracing lists cannot be created at the same time; you must finish the previous raytracing list first using [raytracing_list_end()](#raytracing_list_end).

A simple raytracing operation might look like this (code is not a complete example):

```gdscript
var rd = RenderingDevice.new()
assert(rd.has_feature(RenderingDevice.SUPPORTS_RAYTRACING_PIPELINE))

# Create a BLAS for a mesh.
var geometry = RDAccelerationStructureGeometry.new()
geometry.flags = RenderingDevice.ACCELERATION_STRUCTURE_GEOMETRY_OPAQUE_BIT
geometry.vertex_buffer = vertex_buffer
geometry.vertex_stride = 12
geometry.vertex_format = RenderingDevice.DATA_FORMAT_R32G32B32_SFLOAT
geometry.vertex_count = 3
geometry.index_buffer = index_buffer
geometry.index_count = 3
geometries.push_back(geometry)

blas = rd.blas_create([geometry], 0)

# Create TLAS.
tlas = rd.tlas_create(1, 0)

# Build acceleration structures.
rd.blas_build(blas)

var instance = RDAccelerationStructureInstance.new()
instance.blas = blas

instance.hit_sbt_range = rd.hit_sbt_range_alloc(hit_sbt, 1)
rd.hit_sbt_range_update(hit_sbt, instance.hit_sbt_range, 0, [0])

rd.tlas_build(tlas, [instance])

var raylist = rd.raytracing_list_begin()

# Bind pipeline and uniforms.
rd.raytracing_list_bind_raytracing_pipeline(raylist, raytracing_pipeline)
rd.raytracing_list_bind_uniform_set(raylist, uniform_set, 0)

# Trace rays.
var width = get_viewport().size.x
var height = get_viewport().size.y
rd.raytracing_list_trace_rays(raylist, 0, hit_sbt, width, height, 1)

rd.raytracing_list_end()
```

### raytracing_list_bind_raytracing_pipeline

`void raytracing_list_bind_raytracing_pipeline(raytracing_list: int, raytracing_pipeline: RID)`

**Experimental:** This method may be changed or removed in future versions.

Binds `raytracing_pipeline` to the specified `raytracing_list`.

### raytracing_list_bind_uniform_set

`void raytracing_list_bind_uniform_set(raytracing_list: int, uniform_set: RID, set_index: int)`

**Experimental:** This method may be changed or removed in future versions.

Binds the `uniform_set` to this `raytracing_list`.

### raytracing_list_end

`void raytracing_list_end()`

**Experimental:** This method may be changed or removed in future versions.

Finishes a list of raytracing commands created with the `raytracing_*` methods.

### raytracing_list_set_push_constant

`void raytracing_list_set_push_constant(raytracing_list: int, buffer: PackedByteArray, size_bytes: int)`

**Experimental:** This method may be changed or removed in future versions.

Sets the push constant data to `buffer` for the specified `raytracing_list`. The shader determines how this binary data is used. The buffer's size in bytes must also be specified in `size_bytes` (this can be obtained by calling the [PackedByteArray.size()](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html#class_packedbytearray_method_size) method on the passed `buffer`).

### raytracing_list_trace_rays

`void raytracing_list_trace_rays(raytracing_list: int, raygen_shader_index: int, hit_sbt: RID, width: int, height: int, depth: int)`

**Experimental:** This method may be changed or removed in future versions.

Initializes a raytracing dispatch for `raytracing_list`, launching `width` × `height` × `depth` rays.

`raygen_shader_index` selects the ray generation shader from the pipeline bound with [raytracing_list_bind_raytracing_pipeline()](#raytracing_list_bind_raytracing_pipeline).

`hit_sbt` must use the same pipeline bound to `raytracing_list`.

### raytracing_pipeline_create

`RID raytracing_pipeline_create(raygen_shaders: Array[RDPipelineShader], miss_shaders: Array[RDPipelineShader], hit_groups: Array[RDHitGroup], max_trace_recursion_depth: int)`

**Experimental:** This method may be changed or removed in future versions.

Creates a new raytracing pipeline. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

Each shader must provide the required stage. All stages must use compatible pipeline layouts. The pipeline selects the required stage from each shader.

Input order defines stable indices used by the API:

- `raygen_shaders` is indexed in [raytracing_list_trace_rays()](#raytracing_list_trace_rays).

- `miss_shaders` is indexed in `traceRayEXT`.

- `hit_groups` is indexed in [hit_sbt_range_update()](#hit_sbt_range_update).

### raytracing_pipeline_is_valid

`bool raytracing_pipeline_is_valid(raytracing_pipeline: RID)`

**Experimental:** This method may be changed or removed in future versions.

Returns `true` if the raytracing pipeline specified by the `raytracing_pipeline` RID is valid, `false` otherwise.

### render_pipeline_create

`RID render_pipeline_create(shader: RID, framebuffer_format: int, vertex_format: int, primitive: RenderPrimitive, rasterization_state: RDPipelineRasterizationState, multisample_state: RDPipelineMultisampleState, stencil_state: RDPipelineDepthStencilState, color_blend_state: RDPipelineColorBlendState, dynamic_state_flags: bitfield[PipelineDynamicStateFlags] = 0, for_render_pass: int = 0, specialization_constants: Array[RDPipelineSpecializationConstant] = [])`

Creates a new render pipeline. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when the `shader` is freed.

### render_pipeline_is_valid

`bool render_pipeline_is_valid(render_pipeline: RID)`

Returns `true` if the render pipeline specified by the `render_pipeline` RID is valid, `false` otherwise.

### sampler_create

`RID sampler_create(state: RDSamplerState)`

Creates a new sampler. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### sampler_is_format_supported_for_filter

`bool sampler_is_format_supported_for_filter(format: DataFormat, sampler_filter: SamplerFilter)`

Returns `true` if implementation supports using a texture of `format` with the given `sampler_filter`.

### screen_get_framebuffer_format

`int screen_get_framebuffer_format(screen: int = 0)`

Returns the framebuffer format of the given screen.

**Note:** Only the main **RenderingDevice** returned by [RenderingServer.get_rendering_device()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_get_rendering_device) has a format. If called on a local **RenderingDevice**, this method prints an error and returns [INVALID_ID](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_invalid_id).

### screen_get_height

`int screen_get_height(screen: int = 0)`

Returns the window height matching the graphics API context for the given window ID (in pixels). Despite the parameter being named `screen`, this returns the *window* size. See also [screen_get_width()](#screen_get_width).

**Note:** Only the main **RenderingDevice** returned by [RenderingServer.get_rendering_device()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_get_rendering_device) has a height. If called on a local **RenderingDevice**, this method prints an error and returns [INVALID_ID](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_invalid_id).

### screen_get_width

`int screen_get_width(screen: int = 0)`

Returns the window width matching the graphics API context for the given window ID (in pixels). Despite the parameter being named `screen`, this returns the *window* size. See also [screen_get_height()](#screen_get_height).

**Note:** Only the main **RenderingDevice** returned by [RenderingServer.get_rendering_device()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_get_rendering_device) has a width. If called on a local **RenderingDevice**, this method prints an error and returns [INVALID_ID](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_invalid_id).

### set_resource_name

`void set_resource_name(id: RID, name: String)`

Sets the resource name for `id` to `name`. This is used for debugging with third-party tools such as [RenderDoc](https://renderdoc.org/).

The following types of resources can be named: texture, sampler, vertex buffer, index buffer, uniform buffer, texture buffer, storage buffer, uniform set buffer, shader, render pipeline and compute pipeline. Framebuffers cannot be named. Attempting to name an incompatible resource type will print an error.

**Note:** Resource names are only set when the engine runs in verbose mode ([OS.is_stdout_verbose()](https://docs.godotengine.org/en/stable/classes/class_os.html#class_os_method_is_stdout_verbose) = `true`), or when using an engine build compiled with the `dev_mode=yes` SCons option. The graphics driver must also support the `VK_EXT_DEBUG_UTILS_EXTENSION_NAME` Vulkan extension for named resources to work.

### shader_compile_binary_from_spirv

`PackedByteArray shader_compile_binary_from_spirv(spirv_data: RDShaderSPIRV, name: String = "")`

Compiles a binary shader from `spirv_data` and returns the compiled binary data as a [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html). This compiled shader is specific to the GPU model and driver version used; it will not work on different GPU models or even different driver versions. See also [shader_compile_spirv_from_source()](#shader_compile_spirv_from_source).

`name` is an optional human-readable name that can be given to the compiled shader for organizational purposes.

### shader_compile_spirv_from_source

`RDShaderSPIRV shader_compile_spirv_from_source(shader_source: RDShaderSource, allow_cache: bool = true)`

Compiles a SPIR-V from the shader source code in `shader_source` and returns the SPIR-V as an [RDShaderSPIRV](https://docs.godotengine.org/en/stable/classes/class_rdshaderspirv.html). This intermediate language shader is portable across different GPU models and driver versions, but cannot be run directly by GPUs until compiled into a binary shader using [shader_compile_binary_from_spirv()](#shader_compile_binary_from_spirv).

If `allow_cache` is `true`, make use of the shader cache generated by Godot. This avoids a potentially lengthy shader compilation step if the shader is already in cache. If `allow_cache` is `false`, Godot's shader cache is ignored and the shader will always be recompiled.

### shader_create_from_bytecode

`RID shader_create_from_bytecode(binary_data: PackedByteArray, placeholder_rid: RID = RID())`

Creates a new shader instance from a binary compiled shader. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method. See also [shader_compile_binary_from_spirv()](#shader_compile_binary_from_spirv) and [shader_create_from_spirv()](#shader_create_from_spirv).

### shader_create_from_spirv

`RID shader_create_from_spirv(spirv_data: RDShaderSPIRV, name: String = "")`

Creates a new shader instance from SPIR-V intermediate code. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method. See also [shader_compile_spirv_from_source()](#shader_compile_spirv_from_source) and [shader_create_from_bytecode()](#shader_create_from_bytecode).

### shader_create_placeholder

`RID shader_create_placeholder()`

Create a placeholder RID by allocating an RID without initializing it for use in [shader_create_from_bytecode()](#shader_create_from_bytecode). This allows you to create an RID for a shader and pass it around, but defer compiling the shader to a later time.

### shader_get_vertex_input_attribute_mask

`int shader_get_vertex_input_attribute_mask(shader: RID)`

Returns the internal vertex input mask. Internally, the vertex input mask is an unsigned integer consisting of the locations (specified in GLSL via. `layout(location = ...)`) of the input variables (specified in GLSL by the `in` keyword).

### storage_buffer_create

`RID storage_buffer_create(size_bytes: int, data: PackedByteArray = PackedByteArray(), usage: bitfield[StorageBufferUsage] = 0, creation_bits: bitfield[BufferCreationBits] = 0)`

Creates a [storage buffer](https://vkguide.dev/docs/chapter-4/storage_buffers/) with the specified `data` and `usage`. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### submit

`void submit()`

Pushes the frame setup and draw command buffers then marks the local device as currently processing (which allows calling [sync()](#sync)).

**Note:** Only available in local RenderingDevices.

### sync

`void sync()`

Forces a synchronization between the CPU and GPU, which may be required in certain cases. Only call this when needed, as CPU-GPU synchronization has a performance cost.

**Note:** Only available in local RenderingDevices.

**Note:** [sync()](#sync) can only be called after a [submit()](#submit).

### texture_buffer_create

`RID texture_buffer_create(size_bytes: int, format: DataFormat, data: PackedByteArray = PackedByteArray())`

Creates a new texture buffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### texture_clear

`Error texture_clear(texture: RID, color: Color, base_mipmap: int, mipmap_count: int, base_layer: int, layer_count: int)`

Clears the specified `texture` by replacing all of its pixels with the specified `color`. `base_mipmap` and `mipmap_count` determine which mipmaps of the texture are affected by this clear operation, while `base_layer` and `layer_count` determine which layers of a 3D texture (or texture array) are affected by this clear operation. For 2D textures (which only have one layer by design), `base_layer` must be `0` and `layer_count` must be `1`.

**Note:** `texture` can't be cleared while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to clear this texture.

### texture_copy

`Error texture_copy(from_texture: RID, to_texture: RID, from_pos: Vector3, to_pos: Vector3, size: Vector3, src_mipmap: int, dst_mipmap: int, src_layer: int, dst_layer: int)`

Copies the `from_texture` to `to_texture` with the specified `from_pos`, `to_pos` and `size` coordinates. For 2-dimensional textures, `from_pos` and `to_pos` must have a Z axis of `0`, and `size` must have a Z axis of `1`. Source and destination mipmaps/layers must also be specified, with these parameters being `0` for textures without mipmaps or single-layer textures. Returns [@GlobalScope.OK](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#class_@globalscope_constant_ok) if the texture copy was successful or [@GlobalScope.ERR_INVALID_PARAMETER](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#class_@globalscope_constant_err_invalid_parameter) otherwise.

**Note:** `from_texture` texture can't be copied while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to copy this texture.

**Note:** `from_texture` texture requires the [TEXTURE_USAGE_CAN_COPY_FROM_BIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_usage_can_copy_from_bit) to be retrieved.

**Note:** `to_texture` can't be copied while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to copy this texture.

**Note:** `to_texture` requires the [TEXTURE_USAGE_CAN_COPY_TO_BIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_usage_can_copy_to_bit) to be retrieved.

**Note:** `from_texture` and `to_texture` must be of the same type (color or depth).

### texture_create

`RID texture_create(format: RDTextureFormat, view: RDTextureView, data: Array[PackedByteArray] = [])`

Creates a new texture. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

**Note:** `data` takes an [Array](https://docs.godotengine.org/en/stable/classes/class_array.html) of [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) s. For [TEXTURE_TYPE_1D](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_type_1d), [TEXTURE_TYPE_2D](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_type_2d), and [TEXTURE_TYPE_3D](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_type_3d) types, this array should only have one element, a [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) containing all the data for the texture. For `_ARRAY` and `_CUBE` types, the length should be the same as the number of [RDTextureFormat.array_layers](https://docs.godotengine.org/en/stable/classes/class_rdtextureformat.html#class_rdtextureformat_property_array_layers) in `format`.

**Note:** Not to be confused with [RenderingServer.texture_2d_create()](https://docs.godotengine.org/en/stable/classes/class_renderingserver.html#class_renderingserver_method_texture_2d_create), which creates the Godot-specific [Texture2D](https://docs.godotengine.org/en/stable/classes/class_texture2d.html) resource as opposed to the graphics API's own texture type.

### texture_create_from_extension

`RID texture_create_from_extension(type: TextureType, format: DataFormat, samples: TextureSamples, usage_flags: bitfield[TextureUsageBits], image: int, width: int, height: int, depth: int, layers: int, mipmaps: int = 1)`

Returns an RID for an existing `image` (`VkImage`) with the given `type`, `format`, `samples`, `usage_flags`, `width`, `height`, `depth`, `layers`, and `mipmaps`. This can be used to allow Godot to render onto foreign images.

### texture_create_shared

`RID texture_create_shared(view: RDTextureView, with_texture: RID)`

Creates a shared texture using the specified `view` and the texture information from `with_texture`.

This will be freed automatically when the `with_texture` is freed.

### texture_create_shared_from_slice

`RID texture_create_shared_from_slice(view: RDTextureView, with_texture: RID, layer: int, mipmap: int, mipmaps: int = 1, slice_type: TextureSliceType = 0)`

Creates a shared texture using the specified `view` and the texture information from `with_texture`'s `layer` and `mipmap`. The number of included mipmaps from the original texture can be controlled using the `mipmaps` parameter. Only relevant for textures with multiple layers, such as 3D textures, texture arrays and cubemaps. For single-layer textures, use [texture_create_shared()](#texture_create_shared).

For 2D textures (which only have one layer), `layer` must be `0`.

**Note:** Layer slicing is only supported for 2D texture arrays, not 3D textures or cubemaps.

This will be freed automatically when the `with_texture` is freed.

### texture_get_data

`PackedByteArray texture_get_data(texture: RID, layer: int)`

Returns the `texture` data for the specified `layer` as raw binary data. For 2D textures (which only have one layer), `layer` must be `0`.

**Note:** `texture` can't be retrieved while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to retrieve this texture. Otherwise, an error is printed and an empty [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) is returned.

**Note:** `texture` requires the [TEXTURE_USAGE_CAN_COPY_FROM_BIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_usage_can_copy_from_bit) to be retrieved. Otherwise, an error is printed and an empty [PackedByteArray](https://docs.godotengine.org/en/stable/classes/class_packedbytearray.html) is returned.

**Note:** This method will block the GPU from working until the data is retrieved. Refer to [texture_get_data_async()](#texture_get_data_async) for an alternative that returns the data in more performant way.

### texture_get_data_async

`Error texture_get_data_async(texture: RID, layer: int, callback: Callable)`

Asynchronous version of [texture_get_data()](#texture_get_data). RenderingDevice will call `callback` in a certain amount of frames with the data the texture had at the time of the request.

**Note:** At the moment, the delay corresponds to the amount of frames specified by [ProjectSettings.rendering/rendering_device/vsync/frame_queue_size](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/rendering_device/vsync/frame_queue_size).

**Note:** Downloading large textures can have a prohibitive cost for real-time even when using the asynchronous method due to hardware bandwidth limitations. When dealing with large resources, you can adjust settings such as [ProjectSettings.rendering/rendering_device/staging_buffer/texture_download_region_size_px](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/rendering_device/staging_buffer/texture_download_region_size_px) and [ProjectSettings.rendering/rendering_device/staging_buffer/block_size_kb](https://docs.godotengine.org/en/stable/classes/class_projectsettings.html#class_projectsettings_property_rendering/rendering_device/staging_buffer/block_size_kb) to improve the transfer speed at the cost of extra memory.

```
func _texture_get_data_callback(array):
    value = array.decode_u32(0)

...

rd.texture_get_data_async(texture, 0, _texture_get_data_callback)
```

### texture_get_format

`RDTextureFormat texture_get_format(texture: RID)`

Returns the data format used to create this texture.

### texture_get_native_handle

`int texture_get_native_handle(texture: RID)`

**Deprecated:** Use [get_driver_resource()](#get_driver_resource) with [DRIVER_RESOURCE_TEXTURE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_driver_resource_texture) instead.

Returns the internal graphics handle for this texture object. For use when communicating with third-party APIs mostly with GDExtension.

**Note:** This function returns a `uint64_t` which internally maps to a `GLuint` (OpenGL) or `VkImage` (Vulkan).

### texture_is_discardable

`bool texture_is_discardable(texture: RID)`

Returns `true` if the `texture` is discardable, `false` otherwise. See [RDTextureFormat](https://docs.godotengine.org/en/stable/classes/class_rdtextureformat.html) or [texture_set_discardable()](#texture_set_discardable).

### texture_is_format_supported_for_usage

`bool texture_is_format_supported_for_usage(format: DataFormat, usage_flags: bitfield[TextureUsageBits])`

Returns `true` if the specified `format` is supported for the given `usage_flags`, `false` otherwise.

### texture_is_shared

`bool texture_is_shared(texture: RID)`

Returns `true` if the `texture` is shared, `false` otherwise. See [RDTextureView](https://docs.godotengine.org/en/stable/classes/class_rdtextureview.html).

### texture_is_valid

`bool texture_is_valid(texture: RID)`

Returns `true` if the `texture` is valid, `false` otherwise.

### texture_resolve_multisample

`Error texture_resolve_multisample(from_texture: RID, to_texture: RID)`

Resolves the `from_texture` texture onto `to_texture` with multisample antialiasing enabled. This must be used when rendering a framebuffer for MSAA to work. Returns [@GlobalScope.OK](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#class_@globalscope_constant_ok) if successful, [@GlobalScope.ERR_INVALID_PARAMETER](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#class_@globalscope_constant_err_invalid_parameter) otherwise.

**Note:** `from_texture` and `to_texture` textures must have the same dimension, format and type (color or depth).

**Note:** `from_texture` can't be copied while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to resolve this texture.

**Note:** `from_texture` requires the [TEXTURE_USAGE_CAN_COPY_FROM_BIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_usage_can_copy_from_bit) to be retrieved.

**Note:** `from_texture` must be multisampled and must also be 2D (or a slice of a 3D/cubemap texture).

**Note:** `to_texture` can't be copied while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to resolve this texture.

**Note:** `to_texture` texture requires the [TEXTURE_USAGE_CAN_COPY_TO_BIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_usage_can_copy_to_bit) to be retrieved.

**Note:** `to_texture` texture must **not** be multisampled and must also be 2D (or a slice of a 3D/cubemap texture).

### texture_set_discardable

`void texture_set_discardable(texture: RID, discardable: bool)`

Updates the discardable property of `texture`.

If a texture is discardable, its contents do not need to be preserved between frames. This flag is only relevant when the texture is used as target in a draw list.

This information is used by **RenderingDevice** to figure out if a texture's contents can be discarded, eliminating unnecessary writes to memory and boosting performance.

### texture_update

`Error texture_update(texture: RID, layer: int, data: PackedByteArray)`

Updates texture data with new data, replacing the previous data in place. The updated texture data must have the same dimensions and format. For 2D textures (which only have one layer), `layer` must be `0`. Returns [@GlobalScope.OK](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#class_@globalscope_constant_ok) if the update was successful, [@GlobalScope.ERR_INVALID_PARAMETER](https://docs.godotengine.org/en/stable/classes/class_@globalscope.html#class_@globalscope_constant_err_invalid_parameter) otherwise.

**Note:** Updating textures is forbidden during creation of a draw or compute list.

**Note:** The existing `texture` can't be updated while a draw list that uses it as part of a framebuffer is being created. Ensure the draw list is finalized (and that the color/depth texture using it is not set to [FINAL_ACTION_CONTINUE](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_final_action_continue)) to update this texture.

**Note:** The existing `texture` requires the [TEXTURE_USAGE_CAN_UPDATE_BIT](https://docs.godotengine.org/en/stable/classes/class_renderingdevice.html#class_renderingdevice_constant_texture_usage_can_update_bit) to be updatable.

### tlas_build

`Error tlas_build(tlas: RID, instances: Array[RDAccelerationStructureInstance])`

**Experimental:** This method may be changed or removed in future versions.

Builds the `tlas`. The contents of previous builds are discarded.

Any BLAS provided through the [RDAccelerationStructureInstance.blas](https://docs.godotengine.org/en/stable/classes/class_rdaccelerationstructureinstance.html#class_rdaccelerationstructureinstance_property_blas) member must already have been built using the [blas_build()](#blas_build) method.

The number of instances can be equal to or smaller than the maximum instance count provided in the [tlas_create()](#tlas_create) method.

**Note:** Freeing or rebuilding any of the provided BLASes after this method invalidates the TLAS and requires it to be rebuilt.

### tlas_create

`RID tlas_create(max_instance_count: int, flags: bitfield[AccelerationStructureFlagBits])`

**Experimental:** This method may be changed or removed in future versions.

Creates a new Top-Level Acceleration Structure (TLAS). It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### uniform_buffer_create

`RID uniform_buffer_create(size_bytes: int, data: PackedByteArray = PackedByteArray(), creation_bits: bitfield[BufferCreationBits] = 0)`

Creates a new uniform buffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### uniform_set_create

`RID uniform_set_create(uniforms: Array[RDUniform], shader: RID, shader_set: int)`

Creates a new uniform set. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when the `shader` or any of the RIDs in the `uniforms` is freed.

### uniform_set_is_valid

`bool uniform_set_is_valid(uniform_set: RID)`

Checks if the `uniform_set` is valid, i.e. is owned.

### vertex_array_create

`RID vertex_array_create(vertex_count: int, vertex_format: int, src_buffers: Array[RID], offsets: PackedInt64Array = PackedInt64Array())`

Creates a vertex array based on the specified buffers. Optionally, `offsets` (in bytes) may be defined for each buffer.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

This will be freed automatically when any of the `src_buffers` is freed.

### vertex_buffer_create

`RID vertex_buffer_create(size_bytes: int, data: PackedByteArray = PackedByteArray(), creation_bits: bitfield[BufferCreationBits] = 0)`

Creates a new vertex buffer. It can be accessed with the RID that is returned.

Once finished with your RID, you will want to free the RID using the RenderingDevice's [free_rid()](#free_rid) method.

### vertex_format_create

`int vertex_format_create(vertex_descriptions: Array[RDVertexAttribute])`

Creates a new vertex format with the specified `vertex_descriptions`. Returns a unique vertex format ID corresponding to the newly created vertex format.
