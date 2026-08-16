#include "connectivity/contact_refine.h"
#include <doctest/doctest.h>
#include <map>

using namespace ve;

namespace {

FloodWindow window16() {
	FloodWindow w;
	w.lo = {0, 0, 0};
	w.dim = 16;
	return w;
}

OccupancyGrid air_grid(IVec3 lo, IVec3 hi) {
	OccupancyGrid g;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g.set_cell({x, y, z}, kCellAir, 1);
	return g;
}

void fill(OccupancyGrid *g, IVec3 lo, IVec3 hi, CellState s) {
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) g->set_cell({x, y, z}, s, 2);
}

// A probe the test scripts: every link is a solid contact unless it appears in `thin`.
struct ScriptedProbe : ContactProbe {
	std::map<std::tuple<int, int, int, int>, int> thin;
	int fat = 81;
	mutable int calls = 0;

	int contact_samples(IVec3 c, int axis) const override {
		calls++;
		const auto it = thin.find({c.x, c.y, c.z, axis});
		return it == thin.end() ? fat : it->second;
	}
};

// A floor with a one-cell-thick pillar standing on it and a slab on top of the pillar.
// The link (8,1,8)+y is the pillar's only anchor and therefore the graph's bridge.
OccupancyGrid mushroom() {
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull); // floor, touching the shell
	fill(&g, {8, 2, 8}, {8, 4, 8}, kCellFull);   // stalk
	fill(&g, {6, 5, 6}, {10, 5, 10}, kCellFull); // cap
	return g;
}

} // namespace

TEST_CASE("the only anchor link of a mushroom is found as a bridge") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	CHECK(r.anchored[w.index({8, 5, 8})] == 1); // the cap is currently supported

	std::vector<BridgeLink> bridges;
	find_anchor_bridges(r, ContactRefineConfig{}, &bridges);
	bool found = false;
	for (const BridgeLink &b : bridges)
		if (b.cell == IVec3{8, 1, 8} && b.axis == 1) {
			found = true;
			// Everything above the floor hangs off it: stalk (3) + cap (25).
			CHECK(b.piece_cells == 28);
		}
	CHECK(found);
}

TEST_CASE("a solidly attached slab has no bridge at its base") {
	const FloodWindow w = window16();
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull);
	fill(&g, {5, 2, 5}, {9, 3, 9}, kCellFull); // a block sitting on 25 faces
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	std::vector<BridgeLink> bridges;
	find_anchor_bridges(r, ContactRefineConfig{}, &bridges);
	CHECK(bridges.empty());
}

TEST_CASE("a thin contact is cut and the piece above becomes an island") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);

	ScriptedProbe probe;
	probe.thin[{8, 1, 8, 1}] = 2; // only two of 81 samples are solid across that face
	LinkCuts cuts;
	const int made = refine_anchoring(g, probe, ContactRefineConfig{}, &cuts, &r);
	CHECK(made == 1);
	CHECK(cuts.cut({8, 1, 8}, 1));
	CHECK(r.anchored[w.index({8, 2, 8})] == 0);
	CHECK(r.anchored[w.index({8, 5, 8})] == 0);
	CHECK(r.anchored[w.index({8, 1, 8})] == 1); // the floor is untouched
}

TEST_CASE("a fat contact is left alone and the probe is asked once per bridge") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);
	const int anchored_before = r.anchored_count;

	ScriptedProbe probe; // everything reads 81 solid samples
	LinkCuts cuts;
	CHECK(refine_anchoring(g, probe, ContactRefineConfig{}, &cuts, &r) == 0);
	CHECK(cuts.size() == 0);
	CHECK(r.anchored_count == anchored_before);
	// The mushroom's seedless subtree is a one-cell stalk + cap chain, so every link on
	// that chain is a bridge: cap (25), stalk joints (26, 27) and foot (28). Each is
	// probed exactly once, all read fat, and no further pass is needed.
	CHECK(probe.calls == 4);
}

TEST_CASE("cutting one neck exposes the next one, within the iteration budget") {
	const FloodWindow w = window16();
	OccupancyGrid g = air_grid({-1, -1, -1}, {16, 16, 16});
	fill(&g, {0, 0, 0}, {15, 1, 15}, kCellFull);
	fill(&g, {8, 2, 8}, {8, 8, 8}, kCellFull); // a tall stalk: every link on it is a bridge
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);

	ScriptedProbe probe;
	probe.thin[{8, 1, 8, 1}] = 0; // the foot is hollow
	probe.thin[{8, 4, 8, 1}] = 1; // ...and so is a joint further up
	LinkCuts cuts;
	ContactRefineConfig cfg;
	const int made = refine_anchoring(g, probe, cfg, &cuts, &r);
	// Both cuts land: the foot in the first pass, and the joint either in the same pass
	// (it is also a bridge before anything is cut) or in the next.
	CHECK(made == 2);
	CHECK(r.anchored[w.index({8, 2, 8})] == 0);
}

TEST_CASE("a bridge separating more than max_piece_cells is not a candidate") {
	const FloodWindow w = window16();
	const OccupancyGrid g = mushroom();
	FloodResult r;
	flood_anchored(g, w, nullptr, &r);

	ContactRefineConfig cfg;
	cfg.max_piece_cells = 26;
	std::vector<BridgeLink> bridges;
	find_anchor_bridges(r, cfg, &bridges);

	// The mushroom's seedless stalk + cap chain yields four bridges (25, 26, 27, 28 cells).
	// With the cap at 26, the foot (8,1,8)+y -- the largest piece, 28 cells -- is filtered
	// out, while the cap link (8,4,8)+y (25 cells) still qualifies.
	bool foot_found = false;
	bool cap_found = false;
	for (const BridgeLink &b : bridges) {
		if (b.cell == IVec3{8, 1, 8} && b.axis == 1) foot_found = true;
		if (b.cell == IVec3{8, 4, 8} && b.axis == 1) cap_found = true;
		CHECK(b.piece_cells <= cfg.max_piece_cells);
	}
	CHECK(!foot_found);
	CHECK(cap_found);
}

TEST_CASE("contact_samples_field counts solid samples on the shared face") {
	AnalyticGenerator gen;
	// The face between cells (10,79,10) and (10,80,10) is the plane y = 80 * 0.8 = 64.0 m,
	// which is above the terrain everywhere (the surface is 51.2 +- 10 m), so the only solid
	// on it is what an op puts there.
	CHECK(contact_samples_field(gen, nullptr, 0, {10, 79, 10}, 1, 9) == 0);
	// ...and deep underground every sample is solid.
	CHECK(contact_samples_field(gen, nullptr, 0, {10, 20, 10}, 1, 9) == 81);

	// A ball of fill centred on that face. The face spans x, z in [8.0, 8.8]; a 0.2 m radius
	// meets the plane in a disc of 0.126 m^2 against the face's 0.64 m^2, so a fifth or so
	// of the 81 samples land inside it -- a partial contact, which is the interesting case.
	EditOp add{};
	add.type = kOpSphereAdd;
	add.material = 4;
	add.pos[0] = 8.4f;
	add.pos[1] = 64.0f;
	add.pos[2] = 8.4f;
	add.radius = 0.2f;
	const int n = contact_samples_field(gen, &add, 1, {10, 79, 10}, 1, 9);
	CHECK(n > 0);
	CHECK(n < 81);
}
