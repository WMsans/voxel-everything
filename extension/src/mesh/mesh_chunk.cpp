#include "mesh/mesh_chunk.h"
#include "world/brick_eval.h"
#include <algorithm>
#include <cmath>

namespace ve {

IVec3 chunk_of_brick(IVec3 b) {
	return {floor_div(b.x, kChunkBricks), floor_div(b.y, kChunkBricks),
			floor_div(b.z, kChunkBricks)};
}

IVec3 chunk_of_point(float x, float y, float z) {
	return {static_cast<int>(std::floor(x / kChunkSize)),
			static_cast<int>(std::floor(y / kChunkSize)),
			static_cast<int>(std::floor(z / kChunkSize))};
}

IVec3 region_of_chunk(IVec3 c) {
	constexpr int per = kRegionBricks / kChunkBricks; // 2 chunks per region on each axis
	return {floor_div(c.x, per), floor_div(c.y, per), floor_div(c.z, per)};
}

IVec3 chunk_min_brick(IVec3 c) {
	return {c.x * kChunkBricks, c.y * kChunkBricks, c.z * kChunkBricks};
}

void chunk_world_origin(IVec3 c, float out[3]) {
	out[0] = static_cast<float>(c.x) * kChunkSize;
	out[1] = static_cast<float>(c.y) * kChunkSize;
	out[2] = static_cast<float>(c.z) * kChunkSize;
}

float chunk_distance(IVec3 c, float cx, float cy, float cz) {
	float lo[3];
	chunk_world_origin(c, lo);
	const float p[3] = {cx, cy, cz};
	float d2 = 0.0f;
	for (int a = 0; a < 3; a++) {
		const float over = std::max(0.0f, std::max(lo[a] - p[a], p[a] - (lo[a] + kChunkSize)));
		d2 += over * over;
	}
	return std::sqrt(d2);
}

void op_chunk_range(const EditOp &op, IVec3 *lo, IVec3 *hi) {
	const float r = op.radius + 2.0f * kChunkCellSize;
	const auto cell = [](float v) { return static_cast<int>(std::floor(v / kChunkSize)); };
	*lo = {cell(op.pos[0] - r), cell(op.pos[1] - r), cell(op.pos[2] - r)};
	*hi = {cell(op.pos[0] + r), cell(op.pos[1] + r), cell(op.pos[2] + r)};
}

bool chunk_has_surface(const Generator &gen, const EditOp *ops, int op_count, IVec3 chunk) {
	float o[3];
	chunk_world_origin(chunk, o);
	const float step = kChunkSize / static_cast<float>(kChunkProbeSteps); // 1.6 m
	const float pad = 0.5f * std::sqrt(3.0f) * step * gen.lipschitz();    // ~2.77 m at L = 2
	float mn = 1e30f, mx = -1e30f;
	for (int sz = 0; sz <= kChunkProbeSteps; sz++)
		for (int sy = 0; sy <= kChunkProbeSteps; sy++)
			for (int sx = 0; sx <= kChunkProbeSteps; sx++) {
				const float d = eval_field(gen, ops, op_count, o[0] + sx * step,
						o[1] + sy * step, o[2] + sz * step).sdf;
				mn = std::min(mn, d);
				mx = std::max(mx, d);
			}
	return mn < pad && mx > -pad;
}

} // namespace ve
