#[compute]
#version 460

// Two entry paths behind one shader: level 0 reduces the scene depth into a fixed 256^2
// pyramid (so the CPU readback is resolution-independent), and every level after that
// reduces 2x2 from the level above.
//
// REVERSE-Z (M1 errata 2): near = 1.0, far = 0.0, so the conservative reduction is a MIN.
// It keeps the FARTHEST of the nearest surfaces over a footprint, which is the only value a
// whole node can be tested against without ever wrongly hiding it.
layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1, r32f) writeonly uniform image2D dst;

layout(push_constant, std430) uniform Push {
	ivec4 dims;  // xy = destination size, zw = source size
	ivec4 flags; // x = 1 when the source is the scene depth (level 0), else 0
} pc;

void main() {
	ivec2 p = ivec2(gl_GlobalInvocationID.xy);
	if (any(greaterThanEqual(p, pc.dims.xy))) return;

	float m = 1.0;
	if (pc.flags.x == 1) {
		// The scene is not 256^2 and not a power of two, so each destination texel owns a
		// rectangle of source texels. Reduce all of them: sampling one would let a thin
		// near sliver claim the whole texel and hide what is behind it.
		ivec2 lo = (p * pc.dims.zw) / pc.dims.xy;
		ivec2 hi = ((p + 1) * pc.dims.zw + pc.dims.xy - 1) / pc.dims.xy;
		hi = min(hi, pc.dims.zw);
		for (int y = lo.y; y < hi.y; y++)
			for (int x = lo.x; x < hi.x; x++)
				m = min(m, texelFetch(src, ivec2(x, y), 0).r);
	} else {
		ivec2 s = p * 2;
		m = min(min(texelFetch(src, s, 0).r, texelFetch(src, s + ivec2(1, 0), 0).r),
				min(texelFetch(src, s + ivec2(0, 1), 0).r, texelFetch(src, s + ivec2(1, 1), 0).r));
	}
	imageStore(dst, p, vec4(m));
}
