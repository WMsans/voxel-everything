#[compute]
#version 460

layout(local_size_x = 1) in;

layout(set = 0, binding = 0, std430) readonly buffer Frame {
	int job_count; uint overflow; uint pad0, pad1;
} frame;
layout(set = 0, binding = 1, std430) writeonly buffer Args { uvec4 v; } args;

// brick_gen.comp.glsl runs one workgroup per job, so the group count IS the job count.
void main() {
	args.v = uvec4(uint(max(frame.job_count, 0)), 1u, 1u, 0u);
}
