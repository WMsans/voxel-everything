#include <doctest/doctest.h>
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "lod/lod_reduce.h"
#include "world/brick.h"
#include "world/material_table.h"
#include <vector>

namespace {

constexpr int kN = ve::kLodChunkLattice;
constexpr int kCount = kN * kN * kN;

// An L0 lattice (0.4 m cells) holding a horizontal ground plane at lattice y = 16.5: samples
// y <= 16 are solid rock. `ice_from` relabels solid samples with y >= ice_from as ice, so
// the ice is a slab resting on rock, meeting it with NO surface between them.
void ground(std::vector<uint8_t> *sdf, std::vector<uint16_t> *mat, int ice_from) {
	sdf->assign(kCount, 0);
	mat->assign(kCount, 0);
	for (int z = 0; z < kN; z++)
		for (int y = 0; y < kN; y++)
			for (int x = 0; x < kN; x++) {
				const int i = ve::lod_lattice_index(x, y, z);
				(*sdf)[i] = ve::lod_encode_sdf((float(y) - 16.5f) * 0.4f, 0.4f);
				if (y <= 16) (*mat)[i] = y >= ice_from ? ve::material_id("ice") : ve::material_id("rock");
			}
}

// Relabel every air sample (SDF > 0) with `material`: the stale label a tent-averaged reduce can
// leave on a sample whose SDF rounded positive while its solid taps were ice.
void label_air(const std::vector<uint8_t> &sdf, std::vector<uint16_t> *mat, uint16_t material) {
	for (int i = 0; i < kCount; i++)
		if (ve::decode_sdf(sdf[i]) > 0.0f) (*mat)[i] = material;
}

bool all_axis_y_at(const ve::LodContourResult &r, int u_y, uint16_t material) {
	for (const ve::LodQuad &q : r.quads) {
		ve::LodQuadFields f{};
		ve::lod_quad_unpack(q, &f);
		if (f.axis != 1 || f.u[1] != u_y || f.material != material) return false;
	}
	return !r.quads.empty();
}

} // namespace

TEST_CASE("the opaque lattice is the identity on a chunk with no transparent label") {
	std::vector<uint8_t> sdf, out(kCount);
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 999);
	CHECK_FALSE(ve::lod_has_transparent(sdf.data(), mat.data()));
	ve::lod_opaque_lattice(sdf.data(), mat.data(), 0.4f, out.data());
	CHECK(out == sdf);
}

TEST_CASE("the outside byte decodes as just outside at every level") {
	for (int level = 0; level < 8; level++) {
		const float d = ve::decode_sdf(ve::lod_outside_byte(ve::lod_cell_size(level)));
		CHECK(d > 0.0f);
		CHECK(d <= ve::kSdfRange);
	}
}

TEST_CASE("an air sample with a stale transparent label neither raises the flag nor changes the lattice") {
	std::vector<uint8_t> sdf, out(kCount);
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 999); // every solid sample is rock
	label_air(sdf, &mat, ve::material_id("ice")); // ... but the air above it says ice
	CHECK_FALSE(ve::lod_has_transparent(sdf.data(), mat.data()));
	ve::lod_opaque_lattice(sdf.data(), mat.data(), 0.4f, out.data());
	CHECK(out == sdf);
}

TEST_CASE("the outside byte is half a cell in, so the GPU mirror is a byte-for-byte diff") {
	CHECK(ve::lod_outside_byte(0.4f) == ve::encode_sdf(0.2f));
}

TEST_CASE("an ice slab on rock: opaque mesh at the rock top, shell at the ice top") {
	std::vector<uint8_t> sdf, opaque(kCount);
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 12); // rock y <= 11, ice 12..16, air above
	CHECK(ve::lod_has_transparent(sdf.data(), mat.data()));
	ve::lod_opaque_lattice(sdf.data(), mat.data(), 0.4f, opaque.data());

	// Lattice y = 11 -> 12 is the new crossing; the edge's owning u is lattice index - 1.
	ve::LodContourResult terrain;
	ve::lod_contour(opaque.data(), mat.data(), &terrain);
	CHECK(terrain.quads.size() == size_t(ve::kLodChunkCells * ve::kLodChunkCells));
	CHECK(all_axis_y_at(terrain, 10, ve::material_id("rock")));

	// The shell is the ORIGINAL surface (y = 16 -> 17), and only its transparent quads.
	ve::LodContourResult shell;
	ve::lod_contour(sdf.data(), mat.data(), &shell, true);
	CHECK(shell.quads.size() == size_t(ve::kLodChunkCells * ve::kLodChunkCells));
	CHECK(all_axis_y_at(shell, 15, ve::material_id("ice")));
	CHECK(ve::lod_quads_have_transparent(shell.quads.data(), int(shell.quads.size())));
	CHECK_FALSE(ve::lod_quads_have_transparent(terrain.quads.data(), int(terrain.quads.size())));
}

TEST_CASE("shell_only on a chunk with no transparent label emits nothing, and the default is unchanged") {
	std::vector<uint8_t> sdf;
	std::vector<uint16_t> mat;
	ground(&sdf, &mat, 999);
	ve::LodContourResult shell, plain, plain_again;
	ve::lod_contour(sdf.data(), mat.data(), &shell, true);
	CHECK(shell.quads.empty());
	ve::lod_contour(sdf.data(), mat.data(), &plain);
	ve::lod_contour(sdf.data(), mat.data(), &plain_again, false);
	REQUIRE(plain.quads.size() == plain_again.quads.size());
	for (size_t i = 0; i < plain.quads.size(); i++)
		for (int k = 0; k < 3; k++) CHECK(plain.quads[i].w[k] == plain_again.quads[i].w[k]);
}

TEST_CASE("the shell is appended after the opaque quads and never outgrows a chunk") {
	std::vector<ve::LodQuad> quads(size_t(ve::kLodMaxQuadsPerChunk - 2));
	std::vector<ve::LodQuadNormals> normals(quads.size());
	std::vector<ve::LodQuad> shell(5);
	std::vector<ve::LodQuadNormals> shell_normals(5);
	shell[0].w[0] = 0xABCDu;
	CHECK(ve::lod_append_shell(&quads, &normals, shell, shell_normals));
	CHECK(quads.size() == size_t(ve::kLodMaxQuadsPerChunk));
	CHECK(normals.size() == quads.size());
	CHECK(quads[size_t(ve::kLodMaxQuadsPerChunk - 2)].w[0] == 0xABCDu);

	std::vector<ve::LodQuad> small(3);
	std::vector<ve::LodQuadNormals> small_n(3);
	CHECK_FALSE(ve::lod_append_shell(&small, &small_n, shell, shell_normals));
	CHECK(small.size() == 8);
}
