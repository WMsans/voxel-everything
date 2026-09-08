#include <doctest/doctest.h>
#include "lod/lod_reduce.h"
#include "lod/lod_grid.h"
#include "world/brick.h"
#include <cmath>
#include <vector>

namespace {
// Fills a fine lattice from an analytic function of the LOCAL coordinate, so a test can
// state exactly what it expects the reduction to produce.
void fill_fine(std::vector<uint8_t> *sdf, std::vector<uint16_t> *mat,
		float (*fn)(float, float, float), uint16_t solid_mat) {
	const int n = ve::kLodFineLattice;
	sdf->assign(size_t(n) * n * n, 0);
	mat->assign(size_t(n) * n * n, 0);
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				const float d = fn(ve::lod_fine_local(x), ve::lod_fine_local(y),
						ve::lod_fine_local(z));
				const int i = ve::lod_fine_index(x, y, z);
				(*sdf)[i] = ve::encode_sdf(d);
				(*mat)[i] = d <= 0.0f ? solid_mat : uint16_t(0);
			}
}
} // namespace

// Fine sample j sits at local coordinate (j - 3) / 2, so j = 3 is the chunk origin and the
// target lattice index i reads fine indices 2i, 2i+1, 2i+2.
TEST_CASE("the fine lattice is addressed in half cells") {
	CHECK(ve::lod_fine_local(3) == doctest::Approx(0.0f));
	CHECK(ve::lod_fine_local(1) == doctest::Approx(-1.0f));
	CHECK(ve::lod_fine_local(5) == doctest::Approx(1.0f));
	CHECK(ve::lod_fine_local(4) == doctest::Approx(0.5f));
	// Target index i's tent is centred on 2i+1, which is local coordinate i - 1 -- the
	// mesher's convention (lattice index i holds the sample at local coordinate i - 1).
	for (int i = 0; i < ve::kLodChunkLattice; i++)
		CHECK(ve::lod_fine_local(2 * i + 1) == doctest::Approx(float(i - 1)));
	// The tent for the last target index must stay inside the fine array.
	CHECK(2 * (ve::kLodChunkLattice - 1) + 2 == ve::kLodFineLattice - 1);
}

TEST_CASE("the tent weights are a quarter, a half, a quarter") {
	CHECK(ve::kLodTentWeights[0] == doctest::Approx(0.25f));
	CHECK(ve::kLodTentWeights[1] == doctest::Approx(0.5f));
	CHECK(ve::kLodTentWeights[2] == doctest::Approx(0.25f));
	CHECK(ve::kLodTentWeights[0] + ve::kLodTentWeights[1] + ve::kLodTentWeights[2] ==
			doctest::Approx(1.0f));
}

// A plane is its own average: a linear field must survive the reduction untouched, which is
// what keeps flat ground flat at every level instead of rippling.
TEST_CASE("a linear field reduces to itself") {
	std::vector<uint8_t> fs;
	std::vector<uint16_t> fm;
	fill_fine(&fs, &fm, [](float x, float y, float z) { (void)x; (void)z; return y * 0.01f; }, 2);
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	for (int z = 1; z < ve::kLodChunkLattice - 1; z++)
		for (int y = 1; y < ve::kLodChunkLattice - 1; y++)
			for (int x = 1; x < ve::kLodChunkLattice - 1; x++) {
				const float want = float(y - 1) * 0.01f;
				const float got = ve::decode_sdf(os[ve::lod_lattice_index(x, y, z)]);
				// One encoded step of slack: encode/decode is 8-bit.
				CHECK(std::fabs(got - want) <= 2.0f * ve::kSdfRange / 255.0f + 1e-5f);
			}
}

// The reduction is SYMMETRIC. This is the property that makes a crater survive to L4 and is
// exactly where Voxy's solid-preferring Mipper would be wrong for a destruction demo: a min
// reduction erases air pockets, a max reduction erases spires, an average keeps both.
TEST_CASE("a dent and a bump of equal size survive equally") {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fs(size_t(n) * n * n, ve::encode_sdf(0.3f));
	std::vector<uint16_t> fm(fs.size(), 0);
	// A solid slab in the lower half, with one half-cell bump up and one half-cell dent down.
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				float d = ve::lod_fine_local(y);
				if (x == 20 && z == 20) d -= 0.5f; // bump: more solid
				if (x == 40 && z == 40) d += 0.5f; // dent: more air
				fs[ve::lod_fine_index(x, y, z)] = ve::encode_sdf(d * 0.4f);
				fm[ve::lod_fine_index(x, y, z)] = d <= 0.0f ? uint16_t(2) : uint16_t(0);
			}
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());

	// Sample the reduced field where the bump and the dent landed, against the undisturbed
	// column between them. The two deviations must have equal magnitude and opposite sign.
	// Row 1 keeps every fine y tap used by these output columns inside +/-kSdfRange; sampling
	// a higher row (e.g. 10) would clamp the SDF to the +0.64 m extreme and hide a min/max
	// reduction behind identical encoded values.
	const int by = 1;
	const float base = ve::decode_sdf(os[ve::lod_lattice_index(15, by, 15)]);
	const float bump = ve::decode_sdf(os[ve::lod_lattice_index(10, by, 10)]);
	const float dent = ve::decode_sdf(os[ve::lod_lattice_index(20, by, 20)]);
	CHECK(bump <= base);
	CHECK(dent >= base);
	CHECK(std::fabs((base - bump) - (dent - base)) <= 2.0f * ve::kSdfRange / 255.0f + 1e-5f);
}

// Material is a LABEL: blending two labels is meaningless, so the rule is a vote over the
// solid taps, weighted by the same tent. This is Voxy's Mipper rule (prefer the material
// that actually occupies the volume), and it is what stops distant material boundaries
// dissolving into noise.
TEST_CASE("material is a solidity-weighted majority, never a blend") {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fs(size_t(n) * n * n, ve::encode_sdf(-0.2f)); // all solid
	std::vector<uint16_t> fm(fs.size(), 0);
	// Everything is material 2 except a single tap of material 7 -- one vote cannot win.
	for (size_t i = 0; i < fm.size(); i++) fm[i] = 2;
	fm[ve::lod_fine_index(21, 21, 21)] = 7;
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	CHECK(om[ve::lod_lattice_index(10, 10, 10)] == 2);
	// Never an average of the two ids.
	for (size_t i = 0; i < om.size(); i++) CHECK((om[i] == 2 || om[i] == 7 || om[i] == 0));

	// A majority of 7 in one neighbourhood does win.
	for (int dz = 0; dz < 3; dz++)
		for (int dy = 0; dy < 3; dy++)
			for (int dx = 0; dx < 3; dx++)
				fm[ve::lod_fine_index(20 + dx, 20 + dy, 20 + dz)] = 7;
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	CHECK(om[ve::lod_lattice_index(10, 10, 10)] == 7);
}

// Break caught: clipping physical metres before tent averaging moves a plane's zero set.
// Use the production fine encoder and reducer, then measure the resulting edge crossings
// against the analytic plane, independently of the storage range chosen by the encoder.
namespace {
void check_plane_crossings(float cell, const float normal[3], float height) {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fine(size_t(n) * n * n);
	std::vector<uint16_t> material(fine.size());
	for (int z = 0; z < n; z++)
		for (int y = 0; y < n; y++)
			for (int x = 0; x < n; x++) {
				const float distance = cell * (normal[0] * ve::lod_fine_local(x) +
						normal[1] * ve::lod_fine_local(y) + normal[2] * ve::lod_fine_local(z)) - height;
				const int i = ve::lod_fine_index(x, y, z);
				fine[i] = ve::lod_encode_sdf(distance, cell);
				material[i] = distance <= 0.0f ? 2 : 0;
			}
	const int r = ve::kLodChunkLattice;
	std::vector<uint8_t> reduced(size_t(r) * r * r);
	std::vector<uint16_t> reduced_material(reduced.size());
	ve::lod_reduce_lattice(fine.data(), material.data(), reduced.data(), reduced_material.data());
	float max_plane_error = 0.0f;
	int crossings = 0;
	for (int z = 0; z < r - 1; z++)
		for (int y = 0; y < r - 1; y++)
			for (int x = 0; x < r - 1; x++) {
				const float a = ve::decode_sdf(reduced[ve::lod_lattice_index(x, y, z)]);
				for (int axis = 0; axis < 3; axis++) {
					int end[3] = {x, y, z};
					end[axis]++;
					const float b = ve::decode_sdf(reduced[ve::lod_lattice_index(end[0], end[1], end[2])]);
					if ((a <= 0.0f) == (b <= 0.0f)) continue;
					const float t = a / (a - b);
					const float distance = cell * (normal[0] * float(x - 1) +
							normal[1] * float(y - 1) + normal[2] * float(z - 1) +
							normal[axis] * t) - height;
					max_plane_error = std::fmax(max_plane_error, std::fabs(distance));
					crossings++;
				}
			}
	CAPTURE(cell);
	CAPTURE(normal[0]);
	CAPTURE(normal[1]);
	CAPTURE(normal[2]);
	CAPTURE(height);
	CAPTURE(max_plane_error);
	CHECK(crossings > 0);
	// 2% of a cell allows the two 8-bit quantizations, but not phase-dependent
	// clipping drift. Measure before the separate 5-bit mesh-position quantization.
	CHECK(max_plane_error <= 0.02f * cell);
}
} // namespace

TEST_CASE("a physical plane at 10.123 metres survives coarse SDF storage and reduction") {
	const float normal[3] = {0.0f, 1.0f, 0.0f};
	check_plane_crossings(12.8f, normal, 10.123f); // L5
	check_plane_crossings(51.2f, normal, 10.123f); // L7
}

TEST_CASE("coarse planes preserve their zero set across cell phases and orientations") {
	const float normals[][3] = {
		{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
		{0.6f, 0.8f, 0.0f}, {1.0f / 3.0f, 2.0f / 3.0f, -2.0f / 3.0f},
		{0.904534f, 0.301511f, 0.301511f}
	};
	for (float cell : {0.8f, 3.2f, 12.8f, 51.2f})
		for (const auto &normal : normals)
			for (float phase : {0.017f, 0.123f, 0.37f, 0.79f, 0.983f}) {
				// Move a point on the plane through a cell near the chunk's interior.
				const float height = cell * (8.0f * (normal[0] + normal[1] + normal[2]) + phase);
				check_plane_crossings(cell, normal, height);
			}
}

TEST_CASE("level zero keeps the existing fine SDF bytes") {
	// Break caught: changing the coarse range must not silently retune the finest LoD.
	// Sweep both the narrow band and saturation, including byte midpoints.
	for (int i = -512; i <= 512; i++) {
		const float distance = float(i) * 0.0025f;
		CHECK(ve::lod_encode_sdf(distance, ve::kLodBaseCell) == ve::encode_sdf(distance));
	}
}

// An all-air neighbourhood has no solid taps to vote, and material 0 IS air.
TEST_CASE("air reduces to air") {
	const int n = ve::kLodFineLattice;
	std::vector<uint8_t> fs(size_t(n) * n * n, ve::encode_sdf(0.5f));
	std::vector<uint16_t> fm(fs.size(), 0);
	std::vector<uint8_t> os(size_t(ve::kLodChunkLattice) * ve::kLodChunkLattice *
			ve::kLodChunkLattice);
	std::vector<uint16_t> om(os.size());
	ve::lod_reduce_lattice(fs.data(), fm.data(), os.data(), om.data());
	for (size_t i = 0; i < om.size(); i++) CHECK(om[i] == 0);
	for (size_t i = 0; i < os.size(); i++) CHECK(ve::decode_sdf(os[i]) > 0.0f);
}
