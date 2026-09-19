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
// The trunk/branch shell a tree paints, matching MAT_BARK in shaders/material_table.glslh.
constexpr uint16_t kBandBark = ve::material_id("bark");
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

VE_STAGE_SLOTS(Mesas, p, sdf, mesa_mask, material);
VE_STAGE_PARAMS(Mesas, spacing, lift, plateau);

void stage_mesas(FieldCtx &ctx, const MesasSlots &s, const MesasParams &p,
		const FieldResources &) {
	const float k = 6.2831853f / p.spacing;
	const float u = sinf(ctx.v(s.p)[0] * k) * sinf(ctx.v(s.p)[2] * k);
	const float mask = fmaxf(0.0f, u);
	ctx.f(s.mesa_mask) = mask;
	ctx.f(s.sdf) -= p.lift * mask;
	if (mask > p.plateau && ctx.f(s.sdf) <= 0.0f) ctx.f(s.material) = float(kBandRock);
}

VE_STAGE_SLOTS(Trees, p, sdf, height, material);
VE_STAGE_PARAMS(Trees, cell, density, trunk_height, trunk_radius, branch_radius_min,
		crown_radius, max_slope, hills_amp_a, hills_amp_b, hills_amp_c, relief_amp_a,
		relief_freq_a, relief_amp_b, relief_freq_b);

namespace trees_mirror {
// Line-for-line equivalent of shaders/tree.glslh. Hand-written by decision: transpiling
// stage GLSL to C++ is out of scope (2026-09-17-stage-authoring-design.md §10), and
// tests/test_field_diff.gd is what catches drift between these two copies.
//
// Integer-only hashing, because that test demands bit-identical placement.
constexpr float kJitter = 0.35f;

struct Tp {
	float cell, density, trunk_height, trunk_radius, branch_radius_min, crown_radius, max_slope;
};
struct V3 { float x, y, z; };

inline uint32_t hash(uint32_t x) {
	x = x * 747796405u + 2891336453u;
	const uint32_t w = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
	return (w >> 22u) ^ w;
}
inline uint32_t hash2(int cx, int cz, uint32_t salt) {
	return hash(uint32_t(cx) * 73856093u ^ uint32_t(cz) * 19349663u ^ salt);
}
inline float unit(uint32_t h) { return float(h & 0x00FFFFFFu) / float(0x01000000u); }
inline float snorm(uint32_t h) { return unit(h) * 2.0f - 1.0f; }
inline float mixf(float a, float b, float t) { return a + (b - a) * t; }

inline void cell_xz(int cx, int cz, const Tp &tp, float *ox, float *oz) {
	*ox = (float(cx) + 0.5f) * tp.cell + snorm(hash2(cx, cz, 0x9E37u)) * kJitter * tp.cell;
	*oz = (float(cz) + 0.5f) * tp.cell + snorm(hash2(cx, cz, 0x85EBu)) * kJitter * tp.cell;
}

// Forest gate plus slope and height-band rejection. `slope` is |grad h| at the tree's XZ and
// `h` is the terrain height there above kSurfaceY, both supplied by the caller -- this mirror
// stays terrain-free. Low-frequency lattice noise on the cell grid gives groves and clearings
// rather than an orchard: neighbouring cells share a gate value, so trees arrive in clumps.
inline bool present(int cx, int cz, const Tp &tp, float h, float slope) {
	if (slope > tp.max_slope) return false;
	// SPEC §4 HEIGHT BAND: trees grow on grass and nowhere else. The window mirrors
	// stage_height_bands (shaders/stages/height_bands.field.glslh: rock above 4, grass above
	// 1, dirt below), whose gates are stage text rather than pipeline params, so copying the
	// literals cannot diverge from an author's edit. Kept IDENTICAL to shaders/tree.glslh;
	// test_field_diff.gd pins the two together.
	if (h <= 1.0f || h > 4.0f) return false;
	const float qx = std::floor(float(cx) * 0.25f), qz = std::floor(float(cz) * 0.25f);
	const int gx = int(qx), gz = int(qz);
	float fx = float(cx) * 0.25f - qx, fz = float(cz) * 0.25f - qz;
	fx = fx * fx * (3.0f - 2.0f * fx);
	fz = fz * fz * (3.0f - 2.0f * fz);
	const float a = unit(hash2(gx, gz, 0x1A2Bu));
	const float b = unit(hash2(gx + 1, gz, 0x1A2Bu));
	const float c = unit(hash2(gx, gz + 1, 0x1A2Bu));
	const float d = unit(hash2(gx + 1, gz + 1, 0x1A2Bu));
	const float grove = mixf(mixf(a, b, fx), mixf(c, d, fx), fz);
	return unit(hash2(cx, cz, 0x2F1Du)) < tp.density * (0.35f + 1.3f * grove);
}

struct Tree { V3 base; float height, radius; V3 crown; float crown_r; uint32_t h; bool present; };

// `ground_y`, `ground_h` (terrain height above kSurfaceY at the tree's XZ) and `slope` come
// from the caller (the stage evaluates hills+relief at the tree's XZ). This mirror stays
// terrain-free.
inline Tree at(int cx, int cz, const Tp &tp, float ground_y, float ground_h, float slope) {
	Tree t;
	t.h = hash2(cx, cz, 0xC2B2u);
	t.present = present(cx, cz, tp, ground_h, slope);
	float x, z;
	cell_xz(cx, cz, tp, &x, &z);
	t.base = {x, ground_y, z};
	t.height = tp.trunk_height * (1.0f + 0.25f * snorm(hash(t.h ^ 0x51u)));
	t.radius = tp.trunk_radius * (1.0f + 0.20f * snorm(hash(t.h ^ 0x52u)));
	t.crown_r = fminf(tp.crown_radius,
			tp.crown_radius * t.height / fmaxf(tp.trunk_height, 1e-3f));
	t.crown = {t.base.x, t.base.y + t.height, t.base.z};
	return t;
}

inline V3 lobe(const Tree &t, int i) {
	const uint32_t h = hash(t.h ^ (uint32_t(i) * 0x9E3779B9u));
	const int p = i / 2;
	const float base_az = 6.2831853f * (float(p) / 5.0f) + unit(t.h) * 6.2831853f;
	const float az = base_az + (float(i % 2) * 2.0f - 1.0f) * 0.55f + 0.25f * snorm(h);
	const float el = mixf(0.15f, 0.85f, unit(hash(h ^ 0x77u))) * 1.5707963f;
	const float reach = t.crown_r * mixf(0.42f, 0.62f, unit(hash(h ^ 0x88u)));
	return {t.crown.x + cosf(az) * cosf(el) * reach,
	        t.crown.y + sinf(el) * reach,
	        t.crown.z + sinf(az) * cosf(el) * reach};
}

inline float round_cone(float px, float py, float pz, V3 a, V3 b, float ra, float rb) {
	const V3 ba{b.x - a.x, b.y - a.y, b.z - a.z};
	const float l2 = ba.x * ba.x + ba.y * ba.y + ba.z * ba.z;
	const float rr = ra - rb;
	const float a2 = l2 - rr * rr;
	const float il2 = 1.0f / fmaxf(l2, 1e-8f);
	const V3 pa{px - a.x, py - a.y, pz - a.z};
	const float y = pa.x * ba.x + pa.y * ba.y + pa.z * ba.z;
	const float z = y - l2;
	const V3 w{pa.x * l2 - ba.x * y, pa.y * l2 - ba.y * y, pa.z * l2 - ba.z * y};
	const float x2 = w.x * w.x + w.y * w.y + w.z * w.z;
	const float y2 = y * y * l2;
	const float z2 = z * z * l2;
	auto sgn = [](float v) { return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f); };
	const float k = sgn(rr) * rr * rr * x2;
	if (sgn(z) * a2 * z2 > k) return sqrtf(fmaxf(x2 + z2, 0.0f)) * il2 - rb;
	if (sgn(y) * a2 * y2 < k) return sqrtf(fmaxf(x2 + y2, 0.0f)) * il2 - ra;
	return (sqrtf(fmaxf(x2 * a2 * il2, 0.0f)) + y * rr) * il2 - ra;
}

inline float skeleton(float px, float py, float pz, const Tree &t, const Tp &tp) {
	float d = round_cone(px, py, pz, t.base, t.crown, t.radius, t.radius * 0.5f);
	const V3 fork{t.base.x, t.base.y + t.height * 0.55f, t.base.z};
	for (int i = 0; i < 10; i++) {
		const V3 tip = lobe(t, i);
		const V3 mid{mixf(fork.x, tip.x, 0.55f), mixf(fork.y, tip.y, 0.55f),
				mixf(fork.z, tip.z, 0.55f)};
		const float r_mid = mixf(t.radius * 0.45f, tp.branch_radius_min, 0.5f);
		d = fminf(d, round_cone(px, py, pz, fork, mid, t.radius * 0.45f, r_mid));
		d = fminf(d, round_cone(px, py, pz, mid, tip, r_mid, tp.branch_radius_min));
	}
	return d;
}

inline float bound(float px, float py, float pz, const Tree &t) {
	return round_cone(px, py, pz, t.base, t.crown, t.crown_r, t.crown_r);
}

inline float d_safe(const Tp &tp) {
	return fmaxf(0.0f, (1.5f - kJitter) * tp.cell - tp.crown_radius);
}
} // namespace trees_mirror

void stage_trees(FieldCtx &ctx, const TreesSlots &s, const TreesParams &p,
		const FieldResources &) {
	namespace tm = trees_mirror;
	const tm::Tp tp{p.cell, p.density, p.trunk_height, p.trunk_radius, p.branch_radius_min,
			p.crown_radius, p.max_slope};

	const float px = ctx.v(s.p)[0], py = ctx.v(s.p)[1], pz = ctx.v(s.p)[2];
	const float ground_y = kSurfaceY + ctx.f(s.height);
	// The early-out plane must sit above the tallest geometry AND account for the terrain
	// height field it compares against: in the slab above the ground plane the running
	// Lipschitz bound lets f exceed 1.99 * d_tree, up to h ~= 2.0101 * H where
	// H = trunk_height * 1.25 + crown_radius. Round that slab bound (x1.99) up to 2.05 for
	// margin. Must stay IDENTICAL to the GLSL twin in shaders/stages/trees.field.glslh.
	const float band_top = ground_y + 2.05f * (tp.trunk_height * 1.25f + tp.crown_radius);
	if (py < ground_y - 2.0f || py > band_top) return;

	auto ground_h = [&p](float x, float z) {
		return p.hills_amp_a * sinf(x * 0.11f) * cosf(z * 0.13f)
		     + p.hills_amp_b * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
		     + p.hills_amp_c * sinf(x * 0.23f + z * 0.19f)
		     + p.relief_amp_a * sinf(x * p.relief_freq_a) * cosf(z * p.relief_freq_a)
		     + p.relief_amp_b * sinf(x * p.relief_freq_b) * cosf(z * p.relief_freq_b);
	};
	auto ground_slope = [&p](float x, float z) {
		const float dx = p.hills_amp_a * 0.11f * cosf(x * 0.11f) * cosf(z * 0.13f)
		         + p.hills_amp_b * 0.031f * cosf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
		         + p.hills_amp_c * 0.23f * cosf(x * 0.23f + z * 0.19f)
		         + p.relief_amp_a * p.relief_freq_a * cosf(x * p.relief_freq_a) * cosf(z * p.relief_freq_a)
		         + p.relief_amp_b * p.relief_freq_b * cosf(x * p.relief_freq_b) * cosf(z * p.relief_freq_b);
		const float dz = -p.hills_amp_a * 0.13f * sinf(x * 0.11f) * sinf(z * 0.13f)
		         + p.hills_amp_b * 0.043f * sinf(x * 0.031f + 1.7f) * cosf(z * 0.043f)
		         + p.hills_amp_c * 0.19f * cosf(x * 0.23f + z * 0.19f)
		         - p.relief_amp_a * p.relief_freq_a * sinf(x * p.relief_freq_a) * sinf(z * p.relief_freq_a)
		         - p.relief_amp_b * p.relief_freq_b * sinf(x * p.relief_freq_b) * sinf(z * p.relief_freq_b);
		return sqrtf(dx * dx + dz * dz);
	};

	const int bx = int(std::floor(px / tp.cell));
	const int bz = int(std::floor(pz / tp.cell));
	float d = tm::d_safe(tp);
	for (int dz = -1; dz <= 1; dz++) {
		for (int dx = -1; dx <= 1; dx++) {
			const int cx = bx + dx, cz = bz + dz;
			float tx, tz;
			tm::cell_xz(cx, cz, tp, &tx, &tz);
			const float gh = ground_h(tx, tz);
			// EARLY-OUT 2, the forest gate: one integer hash plus the analytic slope and
			// height band (the rejection spec §4 asks for, inside present()).
			const tm::Tree t = tm::at(cx, cz, tp, kSurfaceY + gh, gh, ground_slope(tx, tz));
			if (!t.present) continue;
			if (tm::bound(px, py, pz, t) >= d) continue;
			d = fminf(d, tm::skeleton(px, py, pz, t, tp));
		}
	}

	if (d < ctx.f(s.sdf)) {
		ctx.f(s.sdf) = d;
		if (d <= 0.0f) ctx.f(s.material) = float(kBandBark);
	}
}

VE_REGISTER_STAGE("ve::stage_hills", Hills, stage_hills);
VE_REGISTER_STAGE("ve::stage_cave", Cave, stage_cave);
VE_REGISTER_STAGE("ve::stage_height_bands", HeightBands, stage_height_bands);
VE_REGISTER_STAGE("ve::stage_relief", Relief, stage_relief);
VE_REGISTER_STAGE("ve::stage_mesas", Mesas, stage_mesas);
VE_REGISTER_STAGE("ve::stage_trees", Trees, stage_trees);

} // namespace ve
