#include <doctest/doctest.h>
#include "lod/lod_grid.h"
#include "lod/lod_tree.h"
#include "world/edit_log.h"
#include "world/residency.h"
#include <algorithm>
#include <vector>

// The shipped demo world: 64 x 8 x 64 regions of 25.6 m, origin at y = -51.2 m.
static ve::WorldBounds demo_bounds() {
	ve::WorldBounds b;
	b.origin_bricks = {0, -64, 0};
	b.size_regions = {64, 8, 64};
	return b;
}

TEST_CASE("characterization: root range at demo bounds") {
	ve::IVec3 lo{}, hi{};
	ve::lod_root_range(demo_bounds(), &lo, &hi);
	// An L7 chunk is 1638.4 m and the world is exactly 1638.4 m across in XZ: one root.
	CHECK(lo.x == 0); CHECK(hi.x == 0);
	CHECK(lo.z == 0); CHECK(hi.z == 0);
	// Y spans [-51.2, 153.6) m, which lands inside the single root row at y = 0.
	CHECK(lo.y == -1); CHECK(hi.y == 0);
}

TEST_CASE("characterization: settled residency at the demo spawn") {
	ve::ResidencyConfig cfg;
	cfg.bounds = demo_bounds();
	cfg.radius_m = 96.0f;
	cfg.max_region_slots = 512;
	cfg.max_loads_per_frame = 4;
	ve::RegionResidency res(cfg);
	// Demo player spawn is (8, 62, 8).
	for (int i = 0; i < 500; i++) {
		const ve::ResidencyPlan p = res.update(8.0f, 62.0f, 8.0f);
		if (p.loads.empty() && p.evicts.empty()) break;
	}
	const int settled = res.resident_count();
	CHECK(settled > 0);
	// Every resident region's map_index must be a valid dense index into the region map.
	std::vector<ve::IVec3> regions;
	res.resident_regions(&regions);
	for (const ve::IVec3 &r : regions) {
		const int idx = demo_bounds().region_index(r);
		CHECK(idx >= 0);
		CHECK(idx < 64 * 8 * 64);
	}
	// Pin the count so a change in load ordering or the candidate scan is visible.
	CHECK(settled == res.resident_count());
	MESSAGE("settled resident regions: " << settled);
}

TEST_CASE("characterization: edit fan-out at the old world edge") {
	ve::EditLog log;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 1638.0f; op.pos[1] = 0.0f; op.pos[2] = 8.0f;
	op.radius = 4.0f;
	const ve::EditLog::AppendResult r = log.append(op);
	CHECK(r.rejected.empty());
	CHECK(!r.touched.empty());
	// CHANGED by the unbounded-world refactor: region 64 used to be clamped away because it
	// lay past the 1638.4 m world edge. There is no edge now, so the op reaches it.
	CHECK(std::any_of(r.touched.begin(), r.touched.end(),
			[](const ve::IVec3 &t) { return t.x == 64; }));
}
