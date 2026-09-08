#include <doctest/doctest.h>
#include "lod/lod_tree.h"
#include "lod/lod_grid.h"

// Regression: nominal ownership bounds do not contain boundary ribbons. Rejecting the
// nominal box removes visible geometry before the GPU has a chance to rasterize it.
TEST_CASE("the tree keeps a chunk whose boundary ribbon enters the frustum") {
	ve::LodTreeConfig cfg;
	cfg.stream_radius_m = 1000.0f;
	cfg.fade_start_m = 0.0f;
	ve::LodTree tree(cfg);
	ve::LodCamera cam{};
	cam.pos[0] = 100.0f; cam.pos[1] = 62.0f; cam.pos[2] = -100.0f;
	// Orthographic diagnostic frustum: x=[90,110], y=[52,72], z=[-110,-90].
	cam.view_proj[0] = cam.view_proj[5] = cam.view_proj[10] = 0.1f;
	cam.view_proj[12] = -10.0f; cam.view_proj[13] = -6.2f;
	cam.view_proj[14] = 10.0f; cam.view_proj[15] = 1.0f;
	std::vector<ve::IVec3> roots;
	ve::lod_roots_in_radius(cam.pos, cfg.stream_radius_m, &roots);
	for (auto c : roots) tree.note_empty(7, c);
	// Nominal X ends at 0. A +X ribbon extends into the diagnostic frustum.
	tree.note_ready(7, {-1, 0, -1}, 7, 1);
	ve::LodWalkResult result;
	tree.walk(cam, nullptr, 1, &result);
	REQUIRE(result.draws.size() == 1);
	CHECK(result.draws[0].coord == ve::IVec3{-1, 0, -1});
}
