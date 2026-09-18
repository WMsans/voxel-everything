// The CPU mirrors of shaders/stages/*.field.glslh. Each function must stay line-for-line
// equivalent to its GLSL twin; tests/test_field_diff.gd is what catches drift.
//
// Each mirror DECLARES the channels and params it binds. PipelineFieldGenerator::create
// resolves those names against the manifest, so a name the manifest does not declare fails
// the load instead of silently binding whatever slot sat at that index.
#include "terrain/stage_library.h"
#include "generator/generator.h"  // ve::kSurfaceY
#include "world/material_table.h"
#include <cmath>

namespace {
// The analytic generator's height bands, by name: rock above 4 m, grass above 1 m, ground below.
constexpr uint16_t kBandRock = ve::material_id("rock");
constexpr uint16_t kBandGrass = ve::material_id("grass_01");
constexpr uint16_t kBandGround = ve::material_id("ground_01");
} // namespace

namespace ve {

VE_STAGE_SLOTS(Hills, p, sdf, height);
VE_STAGE_PARAMS(Hills, amp_a, amp_b, amp_c);

void stage_hills(FieldCtx &ctx, const HillsSlots &s, const HillsParams &p,
		const FieldResources &) {
	const float x = ctx.v(s.p)[0], y = ctx.v(s.p)[1], z = ctx.v(s.p)[2];
	const float h = p.amp_a * sinf(x * 0.11f) * cosf(z * 0.13f)
	              + p.amp_b * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	              + p.amp_c * sinf(x * 0.23f + z * 0.19f);
	ctx.f(s.height) = h;
	ctx.f(s.sdf) = y - kSurfaceY - h;
}

VE_STAGE_SLOTS(Cave, p, sdf);
VE_STAGE_PARAMS(Cave, cx, cz, depth, radius, hills_amp_a, hills_amp_b, hills_amp_c);

void stage_cave(FieldCtx &ctx, const CaveSlots &s, const CaveParams &p,
		const FieldResources &) {
	const float h = p.hills_amp_a * sinf(p.cx * 0.11f) * cosf(p.cz * 0.13f)
	              + p.hills_amp_b * sinf(p.cx * 0.031f + 1.7f) * sinf(p.cz * 0.043f)
	              + p.hills_amp_c * sinf(p.cx * 0.23f + p.cz * 0.19f);
	const float cy = kSurfaceY + h - p.depth;
	const float dx = ctx.v(s.p)[0] - p.cx, dy = ctx.v(s.p)[1] - cy, dz = ctx.v(s.p)[2] - p.cz;
	const float sphere = sqrtf(dx * dx + dy * dy + dz * dz) - p.radius;
	ctx.f(s.sdf) = fmaxf(ctx.f(s.sdf), -sphere);
}

VE_STAGE_SLOTS(HeightBands, sdf, height, material);
VE_STAGE_PARAMS(HeightBands);

void stage_height_bands(FieldCtx &ctx, const HeightBandsSlots &s, const HeightBandsParams &,
		const FieldResources &) {
	if (ctx.f(s.sdf) > 0.0f) { ctx.f(s.material) = 0.0f; return; }
	const float h = ctx.f(s.height);
	ctx.f(s.material) = h > 4.0f ? float(kBandRock) : (h > 1.0f ? float(kBandGrass) : float(kBandGround));
}

VE_STAGE_SLOTS(Relief, p, sdf, height);
VE_STAGE_PARAMS(Relief, amp_a, freq_a, amp_b, freq_b);

void stage_relief(FieldCtx &ctx, const ReliefSlots &s, const ReliefParams &p,
		const FieldResources &) {
	const float x = ctx.v(s.p)[0], z = ctx.v(s.p)[2];
	const float r = p.amp_a * sinf(x * p.freq_a) * cosf(z * p.freq_a)
	              + p.amp_b * sinf(x * p.freq_b) * cosf(z * p.freq_b);
	ctx.f(s.height) += r;
	ctx.f(s.sdf) -= r;
}

VE_REGISTER_STAGE("ve::stage_hills", Hills, stage_hills);
VE_REGISTER_STAGE("ve::stage_cave", Cave, stage_cave);
VE_REGISTER_STAGE("ve::stage_height_bands", HeightBands, stage_height_bands);
VE_REGISTER_STAGE("ve::stage_relief", Relief, stage_relief);

} // namespace ve
