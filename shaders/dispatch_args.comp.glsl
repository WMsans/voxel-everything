#[compute]
#version 460

layout(local_size_x = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Frame {
	int job_count; uint overflow; uint pad0, pad1;
} frame;
layout(set = 0, binding = 1, std430) writeonly buffer Args { uvec4 v; } args;

// 16-byte push constant, never read. Godot's compute_list_add_barrier() restarts the
// command list and REPLAYS the last set push constant against the last bound pipeline;
// the streamer records write_dispatch_args immediately before a barrier, so this
// pipeline must declare a push constant of the size RegionPass::write_dispatch_args
// pushes — otherwise the barrier's replay fails the pipeline's push-size validation.
layout(push_constant, std430) uniform Push {
	ivec4 pad;
} pc;

// brick_gen.comp.glsl runs one workgroup per job, so the group count IS the job count.
void main() {
	args.v = uvec4(uint(max(frame.job_count, 0)), 1u, 1u, 0u);
}
