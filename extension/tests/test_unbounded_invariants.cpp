#include <doctest/doctest.h>
#include "connectivity/occupancy.h"
#include "lod/lod_grid.h"
#include "lod/lod_tree.h"
#include "world/edit_log.h"
#include "world/region_window.h"
#include "world/residency.h"
#include <cmath>
#include <vector>

namespace {
ve::LodCamera cam_at(float x, float y, float z) {
	const float pos[3] = {x, y, z};
	const float fwd[3] = {0.0f, 0.0f, -1.0f};
	const float up[3] = {0.0f, 1.0f, 0.0f};
	return ve::lod_camera_perspective(pos, fwd, up, 1.2217f, 16.0f / 9.0f, 0.1f, 8000.0f,
			2560, 1440);
}
struct NoOcclusion : ve::LodOcclusion {
	bool occluded(const float[3], const float[3]) const override { return false; }
};
} // namespace

TEST_CASE("invariant: ten kilometres of travel does not grow the LoD tree without bound") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1638.4f;
	cfg.evict_frames = 30;
	ve::LodTree tree(cfg);
	NoOcclusion occ;
	int peak = 0;
	for (uint32_t f = 1; f < 400; f++) {
		ve::LodWalkResult out;
		const float x = float(f) * 25.0f; // ~10 km
		tree.walk(cam_at(x, 62.0f, 0.0f), &occ, f, &out);
		std::vector<ve::LodDrawItem> evicted;
		tree.collect_evictions(f, 0, &evicted);
		peak = std::max(peak, tree.node_count());
	}
	// The forest around one camera is tens of roots plus what the walk touched. If the
	// eviction exemption were still unconditional this would be hundreds of stranded nodes.
	CHECK(tree.node_count() < peak * 2);
	CHECK(tree.node_count() < 4000);
}

TEST_CASE("invariant: there is no world edge") {
	// 50 km out on every axis, including deep below the old world floor at y = -51.2 m.
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 50000.0f; op.pos[1] = -50000.0f; op.pos[2] = 50000.0f;
	op.radius = 3.0f;
	const auto r = log.append(op);
	CHECK(!r.oversized);
	CHECK(!r.touched.empty());

	const float cam[3] = {50000.0f, -50000.0f, 50000.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &roots);
	CHECK(!roots.empty());
}

TEST_CASE("invariant: the region lattice is exact at 100 km") {
	// Spec invariant 5. float32 resolves ~8 mm at 100 km, so brick and region quantisation
	// must still round-trip exactly -- this is the documented supported limit.
	const float far = 100000.0f;
	const ve::IVec3 b = ve::brick_of_point(far, 0.0f, far);
	CHECK(b.x == static_cast<int>(std::floor(far / ve::kBrickSize)));
	float lo[3], hi[3];
	ve::brick_world_aabb(b, lo, hi);
	CHECK(lo[0] <= far);
	CHECK(hi[0] >= far);
	// And the region it belongs to is the region the window would index it into.
	const ve::IVec3 r = ve::region_of_point(far, 0.0f, far);
	CHECK(r == ve::region_of_brick(b));
	const ve::RegionWindow w = ve::region_window_centered(far, 0.0f, far, 16);
	CHECK(w.contains(r));
}

TEST_CASE("invariant: an oversized op cannot hang the append path") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.radius = 1.0e5f;
	// The old world box clamped this; without a cap it would iterate ~4.8e14 cells.
	const auto r = log.append(op);
	CHECK(r.oversized);
	CHECK(log.region_count() == 0);
}
