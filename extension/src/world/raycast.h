#pragma once
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include "world/edit_log.h"

namespace ve {

struct RayHit {
	bool hit = false;
	float pos[3] = {0.0f, 0.0f, 0.0f};
	float normal[3] = {0.0f, 0.0f, 0.0f};
	float distance = 0.0f;
};

// Sphere-traces the analytic field (G + each sample point's region ops) with no atlas and no
// GPU. Used by the edit tool to place ops on the main thread without stalling the renderer,
// and as the oracle in tests. dir need not be normalised.
RayHit raycast(const Generator &gen, const EditLog &log, const float origin[3],
		const float dir[3], float max_dist, const VolumeStore *volumes = nullptr);

} // namespace ve
