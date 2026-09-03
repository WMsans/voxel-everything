// The CPU mirrors of shaders/stages/*.field.glslh. Each function must stay line-for-line
// equivalent to its GLSL twin; tests/test_field_diff.gd is what catches drift.
#include "terrain/stage_library.h"
#include "generator/generator.h"  // ve::kSurfaceY
#include <cmath>

namespace ve {

// Slot layout, from PipelineFieldGenerator::create: extra[] holds this stage's declared
// WRITES in order, then its READS in order.
void stage_hills(FieldCtx &ctx, const StageSlots &s, const StageParams &p,
		const FieldResources &) {
	const int sdf = s.extra[0];      // //!out sdf
	const int height = s.extra[1];   // //!out height
	const float x = ctx.v(s.p)[0], y = ctx.v(s.p)[1], z = ctx.v(s.p)[2];
	const float h = p.at(0) * sinf(x * 0.11f) * cosf(z * 0.13f)
	              + p.at(1) * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	              + p.at(2) * sinf(x * 0.23f + z * 0.19f);
	ctx.f(height) = h;
	ctx.f(sdf) = y - kSurfaceY - h;
}

void stage_cave(FieldCtx &ctx, const StageSlots &s, const StageParams &p,
		const FieldResources &) {
	const int sdf = s.extra[0];
	// The cave's params start at index 0 of ITS OWN slice; the hills amplitudes it needs are
	// the literals the GLSL reads through P.hills_*, which are the same constants.
	const float cx = p.at(0), cz = p.at(1), depth = p.at(2), radius = p.at(3);
	const float h = 6.0f * sinf(cx * 0.11f) * cosf(cz * 0.13f)
	              + 3.0f * sinf(cx * 0.031f + 1.7f) * sinf(cz * 0.043f)
	              + 1.0f * sinf(cx * 0.23f + cz * 0.19f);
	const float cy = kSurfaceY + h - depth;
	const float dx = ctx.v(s.p)[0] - cx, dy = ctx.v(s.p)[1] - cy, dz = ctx.v(s.p)[2] - cz;
	const float sphere = sqrtf(dx * dx + dy * dy + dz * dz) - radius;
	ctx.f(sdf) = fmaxf(ctx.f(sdf), -sphere);
}

void stage_height_bands(FieldCtx &ctx, const StageSlots &s, const StageParams &,
		const FieldResources &) {
	const int material = s.extra[0];  // //!out material
	const int sdf = s.extra[1];       // //!in sdf
	const int height = s.extra[2];    // //!in height
	if (ctx.f(sdf) > 0.0f) { ctx.f(material) = 0.0f; return; }
	const float h = ctx.f(height);
	ctx.f(material) = h > 4.0f ? 2.0f : (h > 1.0f ? 1.0f : 3.0f);
}

VE_REGISTER_STAGE("ve::stage_hills", stage_hills);
VE_REGISTER_STAGE("ve::stage_cave", stage_cave);
VE_REGISTER_STAGE("ve::stage_height_bands", stage_height_bands);

} // namespace ve
