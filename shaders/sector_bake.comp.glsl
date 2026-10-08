#[compute]
#version 460

// One sector of the pipeline's map stage (spec §4): every texel, apron included, evaluated at
// its field-space centre and packed as the RG16 the mirrors upload. Compiles in every
// pipeline -- without a map stage the body is empty and SectorContext never exists to
// dispatch it -- because the reload preflight compiles every shader under res://shaders/.
#define FIELD_OP_POOL_BINDING 1
#include "generated/blocks.glslh"
#include "common.glslh"
#include "field.glslh"

layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0, std430) writeonly buffer BakeOut { uint texel[]; } bake_out;
layout(push_constant, std430) uniform Push { SECTOR_BAKE_PUSH_FIELDS } pc;

void main() {
#ifdef VE_SECTOR_MAP
	ivec2 t = ivec2(gl_GlobalInvocationID.xy);
	if (t.x >= SECTOR_STRIDE || t.y >= SECTOR_STRIDE) return;
	vec2 xz = (vec2(pc.sector.xy * SECTOR_TEXELS + t - SECTOR_APRON) + 0.5) * SECTOR_TEXEL_M;
	bake_out.texel[t.y * SECTOR_STRIDE + t.x] = packUnorm2x16(clamp(ve_sector_map(xz), 0.0, 1.0));
#endif
}
