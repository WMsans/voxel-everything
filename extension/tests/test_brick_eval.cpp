#include <doctest/doctest.h>
#include "world/brick_eval.h"
#include "world/palette.h"
#include "world/world_data.h"
#include "generator/generator.h"
#include <cmath>
#include <set>

TEST_CASE("palette_occupancy_order puts the most-used material in slot 0") {
	const uint16_t pal[4] = {3, 1, 2, 0};
	const int counts[4] = {10, 90, 50, 0};
	int order[4] = {};
	ve::palette_occupancy_order(pal, counts, order);
	CHECK(order[0] == 1); // material 1, 90 cells
	CHECK(order[1] == 2); // material 2, 50 cells
	CHECK(order[2] == 0); // material 3, 10 cells
	CHECK(order[3] == 3); // empty slot last
}

TEST_CASE("palette_occupancy_order breaks ties on the lower material id") {
	// Deterministic ties are what let the GPU reproduce this ordering bit for bit.
	const uint16_t pal[4] = {7, 2, 0, 0};
	const int counts[4] = {5, 5, 0, 0};
	int order[4] = {};
	ve::palette_occupancy_order(pal, counts, order);
	CHECK(order[0] == 1); // id 2 < id 7
	CHECK(order[1] == 0);
}

TEST_CASE("eval_field is the generator with the ops applied in order") {
	ve::AnalyticGenerator gen;
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = 8.0f; op.pos[1] = -2.0f; op.pos[2] = 8.0f;
	op.radius = 3.0f;
	const ve::Sample plain = ve::eval_field(gen, nullptr, 0, 8.0f, -2.0f, 8.0f);
	const ve::Sample carved = ve::eval_field(gen, &op, 1, 8.0f, -2.0f, 8.0f);
	CHECK(plain.sdf < 0.0f);            // underground: solid
	CHECK(carved.sdf == doctest::Approx(3.0f)); // blast centre: 3 m of air
	CHECK(carved.material == 0);
}

TEST_CASE("brick_has_surface accepts surface bricks and rejects sky and deep rock") {
	ve::AnalyticGenerator gen;
	// hills() stays inside +-10 m, so a brick at y = +72 m is sky and y = -40 m is rock
	// (the surface sits at 51.2 + hills in [41.2, 61.2] m).
	CHECK_FALSE(ve::brick_has_surface(gen, nullptr, 0, {10, 90, 10}));
	CHECK_FALSE(ve::brick_has_surface(gen, nullptr, 0, {10, -50, 10}));
	// Find the surface brick in this column and check it is accepted.
	int found = 0;
	for (int by = 44; by <= 84; by++)
		if (ve::brick_has_surface(gen, nullptr, 0, {10, by, 10})) found++;
	CHECK(found > 0);
}

TEST_CASE("an edit makes a previously-solid brick a surface brick") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{10, -50, 10};
	REQUIRE_FALSE(ve::brick_has_surface(gen, nullptr, 0, brick));
	float bo[3];
	ve::brick_world_origin(brick, bo);
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = bo[0] + 0.4f; op.pos[1] = bo[1] + 0.4f; op.pos[2] = bo[2] + 0.4f;
	// The sphere must only PARTIALLY cover the brick for its boundary to cross it: a
	// radius of 2.0 swallows the whole 0.8 m cube (half-diagonal 0.693 m), leaving every
	// probe air-side with no zero crossing for the pad probe to detect.
	op.radius = 0.5f;
	CHECK(ve::brick_has_surface(gen, &op, 1, brick));
}

TEST_CASE("cell_state_field follows the generated lattice for a thin carve") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 cell{10, 20, 10};
	ve::EditOp cut{};
	cut.type = ve::kOpSphereSubtract;
	cut.pos[0] = 8.2f;
	cut.pos[1] = 16.2f;
	cut.pos[2] = 8.2f;
	cut.radius = 0.25f;
	ve::BrickEval e{};
	ve::eval_brick(gen, &cut, 1, cell, &e);
	uint8_t mn = 255, mx = 0;
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		mn = std::min(mn, e.brick.sdf[i]);
		mx = std::max(mx, e.brick.sdf[i]);
	}
	const ve::CellState expected = mn > ve::encode_sdf(0.0f)
			? ve::kCellAir
			: (mx <= ve::encode_sdf(0.0f) ? ve::kCellFull : ve::kCellSolid);
		CHECK(ve::cell_state_field(gen, &cut, 1, cell) == expected);
}

TEST_CASE("eval_brick produces a signed lattice, a dominant-first palette and mips") {
	ve::AnalyticGenerator gen;
	// Pick a real surface brick. The pad probe can flag a brick whose top face merely
	// grazes the surface (terrain dips to ~2.54 m at this brick's far corner), so require
	// the evaluated lattice itself to cross zero.
	ve::IVec3 brick{10, 0, 10};
	for (int by = 44; by <= 84; by++) {
		if (!ve::brick_has_surface(gen, nullptr, 0, {10, by, 10})) continue;
		ve::BrickEval probe{};
		ve::eval_brick(gen, nullptr, 0, {10, by, 10}, &probe);
		bool tp = false, tn = false;
		for (int i = 0; i < ve::kBrickSdfCount; i++) {
			const float d = ve::decode_sdf(probe.brick.sdf[i]);
			tp = tp || d > 0.0f;
			tn = tn || d < 0.0f;
		}
		if (tp && tn) { brick = {10, by, 10}; break; }
	}
	ve::BrickEval e{};
	ve::eval_brick(gen, nullptr, 0, brick, &e);

	bool pos = false, neg = false;
	for (int i = 0; i < ve::kBrickSdfCount; i++) {
		const float d = ve::decode_sdf(e.brick.sdf[i]);
		pos = pos || d > 0.0f;
		neg = neg || d < 0.0f;
	}
	CHECK(pos);
	CHECK(neg);
	CHECK(e.brick.palette[0] != 0); // a surface brick always holds a material

	// Slot 0 really is the most-used index.
	int used[4] = {};
	for (int i = 0; i < ve::kBrickVoxelCount; i++) used[ve::get_mat_index(e.brick, i)]++;
	for (int s = 1; s < 4; s++) CHECK(used[0] >= used[s]);

	// The mips agree with a direct recomputation from the very lattice eval_brick wrote.
	ve::BrickMips ref{};
	ve::build_brick_mips(e.brick.sdf, &ref);
	for (int i = 0; i < 8; i++) {
		CHECK(e.mips.mn2[i] == ref.mn2[i]);
		CHECK(e.mips.mx2[i] == ref.mx2[i]);
	}
	// A surface brick must have at least one 2^3 cell reporting a crossing, or the
	// raymarcher's mip skip would step straight over its surface.
	bool any = false;
	for (int i = 0; i < 8; i++) any = any || ve::mip_cell_has_surface(e.mips.mn2[i], e.mips.mx2[i]);
	CHECK(any);
}

TEST_CASE("eval_brick is deterministic and edit ops change its bytes") {
	ve::AnalyticGenerator gen;
	const ve::IVec3 brick{10, 0, 10};
	ve::BrickEval a{}, b{};
	ve::eval_brick(gen, nullptr, 0, brick, &a);
	ve::eval_brick(gen, nullptr, 0, brick, &b);
	for (int i = 0; i < ve::kBrickSdfCount; i++) CHECK(a.brick.sdf[i] == b.brick.sdf[i]);

	float bo[3];
	ve::brick_world_origin(brick, bo);
	ve::EditOp op{};
	op.type = ve::kOpSphereSubtract;
	op.pos[0] = bo[0] + 0.4f; op.pos[1] = bo[1] + 0.4f; op.pos[2] = bo[2] + 0.4f;
	op.radius = 1.0f;
	ve::BrickEval c{};
	ve::eval_brick(gen, &op, 1, brick, &c);
	int differing = 0;
	for (int i = 0; i < ve::kBrickSdfCount; i++) differing += a.brick.sdf[i] != c.brick.sdf[i];
	CHECK(differing > 100);
}

TEST_CASE("WorldData still agrees with eval_brick after the refactor") {
	ve::WorldData w(20, 90, 20);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	REQUIRE(w.active_brick_count() > 0);
	int checked = 0;
	for (int bz = 0; bz < 20 && checked < 8; bz++)
		for (int by = 0; by < 90 && checked < 8; by++)
			for (int bx = 0; bx < 20 && checked < 8; bx++) {
				const int slot = w.brick_slot(bx, by, bz);
				if (slot < 0) continue;
				ve::BrickEval e{};
				ve::eval_brick(gen, nullptr, 0, {bx, by, bz}, &e);
				const ve::Brick &got = w.brick(slot);
				for (int i = 0; i < ve::kBrickSdfCount; i++)
					REQUIRE(got.sdf[i] == e.brick.sdf[i]);
				for (int i = 0; i < ve::kBrickVoxelCount / 4; i++)
					REQUIRE(got.mat[i] == e.brick.mat[i]);
				for (int p = 0; p < ve::kBrickPaletteSize; p++)
					REQUIRE(got.palette[p] == e.brick.palette[p]);
				checked++;
			}
	CHECK(checked == 8);
}
