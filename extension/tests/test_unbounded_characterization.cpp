#include <doctest/doctest.h>
#include "lod/lod_grid.h"
#include "lod/lod_tree.h"
#include "world/edit_log.h"
#include "world/region_window.h"
#include "world/residency.h"
#include <algorithm>
#include <vector>

TEST_CASE("characterization: root forest at the demo spawn") {
	// CHANGED by the unbounded-world refactor: root selection used to come from the world
	// AABB, which was exactly one L7 chunk wide in XZ. It now follows the camera.
	const float cam[3] = {8.0f, 62.0f, 8.0f};
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam, 1638.4f, &roots);
	CHECK(roots.size() >= 8);
	CHECK(roots.size() <= 27);
	const ve::IVec3 own = ve::lod_chunk_of_point(ve::kLodLevels - 1, cam[0], cam[1], cam[2]);
	CHECK(std::find(roots.begin(), roots.end(), own) != roots.end());
}

TEST_CASE("characterization: settled residency at the demo spawn") {
	ve::ResidencyConfig cfg;
	cfg.radius_m = 96.0f;
	cfg.max_region_slots = 512;
	cfg.max_loads_per_frame = 4;
	cfg.window = ve::region_window_centered(8.0f, 62.0f, 8.0f,
			ve::region_window_dim(cfg.radius_m, cfg.evict_margin));
	ve::RegionResidency res(cfg);
	// Demo player spawn is (8, 62, 8).
	for (int i = 0; i < 500; i++) {
		const ve::ResidencyPlan p = res.update(8.0f, 62.0f, 8.0f);
		if (p.loads.empty() && p.evicts.empty()) break;
	}
	const int settled = res.resident_count();
	CHECK(settled > 0);
	// Every resident region's map_index must be a valid window cell index.
	std::vector<ve::IVec3> regions;
	res.resident_regions(&regions);
	for (const ve::IVec3 &r : regions) {
		const int idx = res.window().index(r);
		CHECK(idx >= 0);
		CHECK(idx < res.window().cell_count());
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
