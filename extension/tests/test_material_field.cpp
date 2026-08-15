// The renderer's hit point routinely lands on the AIR side of the surface: the march stops
// at d < 0.002 and the secant refinement can leave p just outside. material_at() then reads
// the nearest lattice cell, which is an air cell. Air cells were never assigned a material,
// so their packed 2-bit index stayed 0 -- and index 0 is indistinguishable from palette slot
// 0, which holds the brick's FIRST material. In a brick holding one material that is
// accidentally right; in a brick straddling a material boundary it is wrong, producing the
// dithered fringe along the grass/dirt and grass/rock borders.
//
// The material field must therefore be TOTAL: every cell an eye ray can resolve against
// carries the material of the surface nearest to it, so an air-side lookup is still correct.
#include <doctest/doctest.h>
#include "world/world_data.h"
#include "world/palette.h"
#include "generator/generator.h"
#include <algorithm>
#include <cmath>

namespace {

float hills(float x, float z) { // test oracle, mirrors AnalyticGenerator
	return 6.0f * sinf(x * 0.11f) * cosf(z * 0.13f)
	     + 3.0f * sinf(x * 0.031f + 1.7f) * sinf(z * 0.043f)
	     + 1.0f * sinf(x * 0.23f + z * 0.19f);
}

// Material of the terrain surface in the column through (x, z), asked of the generator so
// the band thresholds themselves are not duplicated here. The surface now sits at
// kSurfaceY + hills, so the probe is sampled one voxel below that.
uint16_t column_material(const ve::Generator &gen, float x, float z) {
	return gen.sample(x, ve::kSurfaceY + hills(x, z) - 0.01f, z).material;
}

} // namespace

// Every cell within reach of a surface hit must resolve to that surface's material. This is
// the property the fringe violates: near-surface AIR cells fall through to palette[0].
TEST_CASE("near-surface cells carry the material of the surface beside them") {
	ve::WorldData w(48, 90, 48);
	ve::AnalyticGenerator gen;
	w.generate(gen);

	long tested = 0, wrong = 0, air_cells = 0;
	for (int bz = 0; bz < 48; bz++)
		for (int by = 0; by < 90; by++)
			for (int bx = 0; bx < 48; bx++) {
				const int slot = w.brick_slot(bx, by, bz);
				if (slot < 0) continue;
				const ve::Brick &b = w.brick(slot);
				if (b.palette[0] == 0) continue; // no solid at all in this brick
				for (int vz = 0; vz < ve::kBrickVoxels; vz++)
					for (int vy = 0; vy < ve::kBrickVoxels; vy++)
						for (int vx = 0; vx < ve::kBrickVoxels; vx++) {
							const float d = ve::decode_sdf(b.sdf[ve::sdf_index(vx, vy, vz)]);
							// Only cells a hit point can round to: within ~1 voxel of the surface.
							if (std::fabs(d) > 1.2f * ve::kVoxelSize) continue;
							const float wx = (bx * ve::kBrickVoxels + vx) * ve::kVoxelSize;
							const float wz = (bz * ve::kBrickVoxels + vz) * ve::kVoxelSize;
							const uint16_t truth = column_material(gen, wx, wz);
							if (truth == 0) continue;
							// The band boundary is only representable to one voxel; skip its
							// neighbourhood so every remaining cell has one right answer.
							bool pure = true;
							for (int dx = -1; dx <= 1 && pure; dx++)
								for (int dz = -1; dz <= 1 && pure; dz++)
									pure = column_material(gen, wx + dx * ve::kVoxelSize,
									                            wz + dz * ve::kVoxelSize) == truth;
							if (!pure) continue;
							if (d > 0.0f) air_cells++;
							tested++;
							if (b.palette[ve::get_mat_index(b, ve::voxel_index(vx, vy, vz))] != truth) wrong++;
						}
			}
	CAPTURE(tested);
	CAPTURE(air_cells);
	CAPTURE(wrong);
	REQUIRE(tested > 100000);
	REQUIRE(air_cells > 10000); // the failure mode must actually be exercised
	CHECK(wrong == 0);
}

TEST_CASE("the fill introduces no material the brick did not already contain") {
	ve::WorldData w(40, 90, 40);
	ve::AnalyticGenerator gen;
	w.generate(gen);
	for (int slot = 0; slot < w.active_brick_count(); slot++) {
		const ve::Brick &b = w.brick(slot);
		int distinct = 0;
		for (int p = 0; p < ve::kBrickPaletteSize; p++) {
			const uint16_t id = b.palette[p];
			if (id == 0) continue;
			distinct++;
			CHECK((id == 1 || id == 2 || id == 3)); // only materials this generator emits
		}
		// Filling air cells must not invent materials: a brick still holds at most the
		// handful of bands its own solid voxels sampled.
		CHECK(distinct <= 3);
	}
}
