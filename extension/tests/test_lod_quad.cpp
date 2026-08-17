#include <doctest/doctest.h>
#include "lod/lod_quad.h"
#include "lod/lod_grid.h"
#include <cmath>

TEST_CASE("the record is exactly twelve bytes") {
	CHECK(sizeof(ve::LodQuad) == 12);
	CHECK(ve::kLodQuadBytes == 12);
	CHECK(ve::kLodOffsetBits == 5);
	CHECK(ve::kLodOffsetMax == 31);
}

// Every field survives a round trip at its extremes. The layout spans three uint32s, so the
// fields that straddle a word boundary (the corner offsets, the material) are the ones that
// a naive shift would silently truncate.
TEST_CASE("every field round-trips at its extremes") {
	ve::LodQuadFields f{};
	f.u[0] = 31; f.u[1] = 0; f.u[2] = 17;
	f.axis = 2;
	f.sign = 1;
	f.material = 0xBEEF;
	f.double_sided = 1;
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++) f.offset[k][a] = static_cast<uint8_t>((k * 3 + a) % 32);
	f.offset[0][0] = 0;
	f.offset[3][2] = 31;

	ve::LodQuad q{};
	ve::lod_quad_pack(f, &q);
	ve::LodQuadFields g{};
	ve::lod_quad_unpack(q, &g);

	CHECK(g.u[0] == f.u[0]);
	CHECK(g.u[1] == f.u[1]);
	CHECK(g.u[2] == f.u[2]);
	CHECK(g.axis == f.axis);
	CHECK(g.sign == f.sign);
	CHECK(g.material == f.material);
	CHECK(g.double_sided == f.double_sided);
	for (int k = 0; k < 4; k++)
		for (int a = 0; a < 3; a++) CHECK(g.offset[k][a] == f.offset[k][a]);
}

// An exhaustive sweep of each field on its own, so a wrong shift cannot hide behind another
// field's bits happening to be zero.
TEST_CASE("fields do not bleed into each other") {
	for (int axis = 0; axis < 3; axis++) {
		for (int v = 0; v < 32; v++) {
			ve::LodQuadFields f{};
			f.axis = static_cast<uint8_t>(axis);
			f.u[0] = static_cast<uint8_t>(v);
			f.offset[2][1] = static_cast<uint8_t>(31 - v);
			f.material = static_cast<uint16_t>(v * 2111u);
			ve::LodQuad q{};
			ve::lod_quad_pack(f, &q);
			ve::LodQuadFields g{};
			ve::lod_quad_unpack(q, &g);
			CHECK(g.axis == f.axis);
			CHECK(g.u[0] == f.u[0]);
			CHECK(g.u[1] == 0);
			CHECK(g.u[2] == 0);
			CHECK(g.offset[2][1] == f.offset[2][1]);
			CHECK(g.offset[2][0] == 0);
			CHECK(g.material == f.material);
			CHECK(g.sign == 0);
			CHECK(g.double_sided == 0);
		}
	}
}

// The four cells around an edge share their coordinate along the edge axis and differ by
// -1/0 in the two perpendicular ones. That is what makes 5-bit offsets sufficient.
TEST_CASE("the four corner cells occupy a 1x2x2 block around the owned edge") {
	for (int axis = 0; axis < 3; axis++) {
		ve::LodQuadFields f{};
		f.axis = static_cast<uint8_t>(axis);
		f.u[0] = 4; f.u[1] = 7; f.u[2] = 9;
		int seen[4][3];
		for (int k = 0; k < 4; k++) ve::lod_quad_corner_cell(f, k, seen[k]);
		const int L[3] = {f.u[0] + 1, f.u[1] + 1, f.u[2] + 1};
		for (int k = 0; k < 4; k++) {
			CHECK(seen[k][axis] == L[axis]);
			for (int a = 0; a < 3; a++) {
				CHECK(seen[k][a] >= L[a] - 1);
				CHECK(seen[k][a] <= L[a]);
			}
		}
		// The four are distinct.
		for (int i = 0; i < 4; i++)
			for (int j = i + 1; j < 4; j++)
				CHECK(!(seen[i][0] == seen[j][0] && seen[i][1] == seen[j][1] &&
						seen[i][2] == seen[j][2]));
	}
}

// Decoded positions must match the mesher's own vertex formula,
// origin + (m - 1 + frac) * cell (shaders/mesh_cells.comp.glsl), to within the quantiser.
TEST_CASE("corner positions match the mesher formula within the quantiser") {
	const float origin[3] = {12.8f, -25.6f, 3.2f};
	const float cell = ve::lod_cell_size(1);
	ve::LodQuadFields f{};
	f.axis = 1;
	f.u[0] = 3; f.u[1] = 4; f.u[2] = 5;
	f.offset[0][0] = 0;  f.offset[0][1] = 16; f.offset[0][2] = 31;
	f.offset[1][0] = 31; f.offset[1][1] = 0;  f.offset[1][2] = 8;
	f.offset[2][0] = 15; f.offset[2][1] = 15; f.offset[2][2] = 15;
	f.offset[3][0] = 7;  f.offset[3][1] = 24; f.offset[3][2] = 1;
	for (int k = 0; k < 4; k++) {
		int m[3];
		ve::lod_quad_corner_cell(f, k, m);
		float p[3];
		ve::lod_quad_corner_pos(f, k, origin, cell, p);
		for (int a = 0; a < 3; a++) {
			const float frac = float(f.offset[k][a]) / float(ve::kLodOffsetMax);
			const float want = origin[a] + (float(m[a]) - 1.0f + frac) * cell;
			CHECK(p[a] == want);
		}
	}
}

// The quantiser's worst case is half a step, and a step is 1/31 of a cell. Spec section 3.1
// claims 1/32 of a cell "at every level"; this pins the real bound so the claim cannot rot.
TEST_CASE("quantisation error is under one thirty-second of a cell") {
	const float step = 1.0f / float(ve::kLodOffsetMax);
	CHECK(0.5f * step < 1.0f / 32.0f);
	for (float frac = 0.0f; frac <= 1.0f; frac += 0.001f) {
		const uint8_t o = ve::lod_quantise_offset(frac);
		CHECK(o <= ve::kLodOffsetMax);
		const float back = float(o) / float(ve::kLodOffsetMax);
		CHECK(std::fabs(back - frac) <= 0.5f * step + 1e-6f);
	}
	// The endpoints are exact, so adjacent quads meeting at a cell corner cannot separate.
	CHECK(ve::lod_quantise_offset(0.0f) == 0);
	CHECK(ve::lod_quantise_offset(1.0f) == ve::kLodOffsetMax);
}
