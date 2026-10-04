#include <doctest/doctest.h>
#include "generator/volume_set.h"
#include "transparency/shell_grid.h"
#include "world/brick.h"
#include "world/material_table.h"
#include "world/override_store.h"
#include <algorithm>

namespace {
bool has(const std::vector<ve::IVec3> &v, ve::IVec3 c) {
	return std::any_of(v.begin(), v.end(), [c](ve::IVec3 o) {
		return o.x == c.x && o.y == c.y && o.z == c.z; });
}
ve::EditOp sphere(uint32_t type, uint16_t material, float x, float y, float z, float r) {
	ve::EditOp op{};
	op.type = type; op.material = material;
	op.pos[0] = x; op.pos[1] = y; op.pos[2] = z; op.radius = r;
	return op;
}
} // namespace

TEST_CASE("shell chunks are 3.2 m and tile negative space") {
	CHECK(ve::kShellChunkSize == doctest::Approx(3.2f));
	const ve::IVec3 c = ve::shell_chunk_of_point(-0.1f, 3.3f, 6.4f);
	CHECK(c.x == -1); CHECK(c.y == 1); CHECK(c.z == 2);
	float o[3];
	ve::shell_chunk_origin(c, o);
	CHECK(o[0] == doctest::Approx(-3.2f));
	CHECK(o[1] == doctest::Approx(3.2f));
}

// A near-shell chunk IS a finest-level LoD chunk: same 32 cells of 0.1 m, same origin. The
// shell job's level is therefore a real level, and lod_chunk_origin() places it correctly.
TEST_CASE("a shell chunk is the finest LoD level's chunk") {
	CHECK(ve::kShellLevel == ve::kLodMinLevel);
	for (const ve::IVec3 c : {ve::IVec3{0, 0, 0}, ve::IVec3{-3, 17, 5}}) {
		float a[3], b[3];
		ve::shell_chunk_origin(c, a);
		ve::lod_chunk_origin(ve::kShellLevel, c, b);
		for (int i = 0; i < 3; i++) CHECK(a[i] == doctest::Approx(b[i]));
	}
}

TEST_CASE("only transparent ops make boxes") {
	const float lo[3] = {-100, -100, -100}, hi[3] = {100, 100, 100};
	const ve::EditOp ops[] = {
		sphere(ve::kOpSphereAdd, ve::material_id("ice"), 10, 10, 10, 1.0f),
		sphere(ve::kOpSphereAdd, ve::material_id("rock"), 20, 10, 10, 1.0f),
		sphere(ve::kOpSpherePaint, ve::material_id("ice_crack"), 30, 10, 10, 2.0f),
		sphere(ve::kOpSphereSubtract, ve::material_id("ice"), 40, 10, 10, 2.0f),
	};
	std::vector<ve::ShellBox> boxes;
	ve::transparent_boxes(ops, 4, nullptr, nullptr, lo, hi, &boxes);
	REQUIRE(boxes.size() == 2);
	CHECK(boxes[0].lo[0] == doctest::Approx(9.0f));
	CHECK(boxes[1].hi[0] == doctest::Approx(32.0f));
}

TEST_CASE("an override brick holding a transparent label makes a box; an opaque one does not") {
	ve::OverrideStore store(4);
	const ve::IVec3 a{1, 2, 3}, b{5, 2, 3};
	ve::OverrideBrick *ba = store.data(store.acquire(a));
	ve::OverrideBrick *bb = store.data(store.acquire(b));
	std::fill(std::begin(ba->mat), std::end(ba->mat), uint8_t(ve::material_id("rock")));
	std::fill(std::begin(bb->mat), std::end(bb->mat), uint8_t(ve::material_id("rock")));
	bb->mat[100] = uint8_t(ve::material_id("ice"));
	const float lo[3] = {-100, -100, -100}, hi[3] = {100, 100, 100};
	std::vector<ve::ShellBox> boxes;
	ve::transparent_boxes(nullptr, 0, nullptr, &store, lo, hi, &boxes);
	REQUIRE(boxes.size() == 1);
	CHECK(boxes[0].lo[0] == doctest::Approx(5 * ve::kBrickSize));
	// Outside the area of interest: no box.
	const float far_lo[3] = {500, 500, 500}, far_hi[3] = {600, 600, 600};
	boxes.clear();
	ve::transparent_boxes(nullptr, 0, nullptr, &store, far_lo, far_hi, &boxes);
	CHECK(boxes.empty());
}

TEST_CASE("candidates cover a box and stop at the radius") {
	std::vector<ve::ShellBox> boxes = {{{0.5f, 0.5f, 0.5f}, {1.5f, 1.5f, 1.5f}},
			{{200.0f, 0.0f, 0.0f}, {201.0f, 1.0f, 1.0f}}};
	const float cam[3] = {0, 0, 0};
	std::vector<ve::IVec3> out;
	ve::shell_candidates(boxes, cam, 50.0f, &out);
	CHECK(has(out, ve::IVec3{0, 0, 0}));
	CHECK_FALSE(has(out, ve::IVec3{62, 0, 0})); // the far box is past the radius
	// A box touching a chunk face reaches the neighbour (one cell of pad).
	boxes = {{{3.15f, 1.0f, 1.0f}, {3.19f, 1.1f, 1.1f}}};
	ve::shell_candidates(boxes, cam, 50.0f, &out);
	CHECK(has(out, ve::IVec3{0, 0, 0}));
	CHECK(has(out, ve::IVec3{1, 0, 0}));
}

TEST_CASE("the grid requests unknown chunks nearest first and rebuilds dirty ones") {
	ve::ShellGrid g;
	std::vector<ve::IVec3> evicted, req;
	g.set_candidates({ve::IVec3{5, 0, 0}, ve::IVec3{1, 0, 0}}, &evicted);
	CHECK(evicted.empty());
	const float cam[3] = {0, 0, 0};
	g.requests(cam, 8, &req);
	REQUIRE(req.size() == 2);
	CHECK(req[0].x == 1);
	g.note_building(req[0]);
	g.note_result(req[0], true);
	CHECK(g.state(ve::IVec3{1, 0, 0}) == ve::kShellReady);
	g.requests(cam, 8, &req);
	REQUIRE(req.size() == 1);
	CHECK(req[0].x == 5);
	// An edit over the ready chunk makes it requestable again, still drawable meanwhile.
	const float lo[3] = {3.3f, 0.1f, 0.1f}, hi[3] = {3.4f, 0.2f, 0.2f};
	g.mark_dirty(lo, hi);
	CHECK(g.dirty(ve::IVec3{1, 0, 0}));
	CHECK(g.state(ve::IVec3{1, 0, 0}) == ve::kShellReady);
	g.requests(cam, 1, &req);
	REQUIRE(req.size() == 1);
	CHECK(req[0].x == 1);
	// A build that was in flight when an edit landed stays requestable after its result.
	g.note_building(ve::IVec3{1, 0, 0});
	g.mark_dirty(lo, hi);
	g.note_result(ve::IVec3{1, 0, 0}, false);
	CHECK(g.state(ve::IVec3{1, 0, 0}) == ve::kShellEmpty);
	CHECK(g.dirty(ve::IVec3{1, 0, 0}));
}

TEST_CASE("chunks that leave the candidate set are reported evicted exactly once") {
	ve::ShellGrid g;
	std::vector<ve::IVec3> evicted;
	g.set_candidates({ve::IVec3{1, 0, 0}, ve::IVec3{2, 0, 0}}, &evicted);
	g.set_candidates({ve::IVec3{2, 0, 0}}, &evicted);
	REQUIRE(evicted.size() == 1);
	CHECK(evicted[0].x == 1);
	evicted.clear();
	g.set_candidates({ve::IVec3{2, 0, 0}}, &evicted);
	CHECK(evicted.empty());
	CHECK(g.size() == 1);
}

TEST_CASE("thickness: a slab, two layers, a ray ending on ground, the camera inside, the sky") {
	// One slab: front at 5, back at 7, ground at 20.
	CHECK(ve::shell_thickness(7.0f - 5.0f, 0.0f, 20.0f, 5.0f, 4.0f) == doctest::Approx(2.0f));
	// Two slabs: [5,7] and [9,12].
	CHECK(ve::shell_thickness((7.0f + 12.0f) - (5.0f + 9.0f), 0.0f, 20.0f, 5.0f, 4.0f) ==
			doctest::Approx(5.0f));
	// Enters at 5 and ends on ground at 8 inside the ice: one front, no back.
	CHECK(ve::shell_thickness(-5.0f, 1.0f, 8.0f, 5.0f, 4.0f) == doctest::Approx(3.0f));
	// Camera inside: the clear value is a front at 0, the only face is a back at 6.
	CHECK(ve::shell_thickness(6.0f, 0.0f, 20.0f, 0.0f, 4.0f) == doctest::Approx(6.0f));
	// A cut-off shell against the sky: exits sky_thickness past the front.
	CHECK(ve::shell_thickness(-5.0f, 1.0f, 0.0f, 5.0f, 4.0f) == doctest::Approx(4.0f));
	// Never negative, whatever a clipped face left behind.
	CHECK(ve::shell_thickness(-3.0f, -1.0f, 2.0f, 0.0f, 4.0f) == 0.0f);
}

TEST_CASE("an island's shell is split into 32-cell blocks and only holds transparent quads") {
	ve::VolumeData v;
	v.dim = ve::kIslandDim;
	v.sdf.assign(size_t(v.voxel_count()), ve::encode_sdf(0.3f));
	v.mat.assign(size_t(v.voxel_count()), 0);
	// A solid ball of radius 20 voxels in the middle; its upper half is ice, lower rock.
	const float voxel = ve::kIslandVoxelFine;
	for (int z = 0; z < v.dim; z++)
		for (int y = 0; y < v.dim; y++)
			for (int x = 0; x < v.dim; x++) {
				const float dx = x - 32.0f, dy = y - 32.0f, dz = z - 32.0f;
				const float d = (std::sqrt(dx * dx + dy * dy + dz * dz) - 20.0f) * voxel;
				const int i = ve::VolumeSet::voxel_index(v.dim, x, y, z);
				v.sdf[size_t(i)] = ve::encode_sdf(d);
				if (d <= 0.0f)
					v.mat[size_t(i)] = uint8_t(ve::material_id(y >= 32 ? "ice" : "rock"));
			}
	CHECK(ve::volume_has_transparent(v));
	const float origin[3] = {-1.6f, -1.6f, -1.6f};
	std::vector<ve::IslandShellBlock> blocks;
	ve::island_shell_blocks(v, origin, voxel, &blocks);
	REQUIRE(!blocks.empty());
	CHECK(blocks.size() <= 8);
	size_t quads = 0;
	for (const ve::IslandShellBlock &b : blocks) {
		CHECK(b.quads.size() == b.normals.size());
		CHECK(b.origin_local[1] >= origin[1] - 1e-4f); // lower blocks are all rock: omitted
		for (const ve::LodQuad &q : b.quads) {
			ve::LodQuadFields f{};
			ve::lod_quad_unpack(q, &f);
			CHECK(ve::material_transparent(uint16_t(f.material)));
		}
		quads += b.quads.size();
	}
	CHECK(quads > 100);

	std::fill(v.mat.begin(), v.mat.end(), uint8_t(ve::material_id("rock")));
	CHECK_FALSE(ve::volume_has_transparent(v));
	ve::island_shell_blocks(v, origin, voxel, &blocks);
	CHECK(blocks.empty());
}

// Carried from Task 6 and Task 8 reviews: the chunk record meta[1] carries the island slot
// and NOTHING else reads it -- its only consumer is shell.vert.glsl. Pin the layout that
// shader decodes, because a mis-decode does not fail loudly: the pages are still valid quads,
// they are just placed in the wrong space, so every island shell vanishes or lands metres away.
// NOT a full pin: these assertions restate shell.vert.glsl's own expressions in C++, against
// the same ve::island_shell_flags() the production path calls, so changing the SHADER (e.g.
// `>> 8` to `>> 7`) would leave this test green. What it does hold is the C++ side: the
// encode, the disjointness of the two fields, and the terrain-page word. The decode itself is
// pinned by the GPU test, which draws an island's shell in the right place.
TEST_CASE("an island shell page's chunk flags decode to its slot and the near bit") {
	for (const int slot : {0, 1, 7, 31}) {
		const uint32_t flags = ve::island_shell_flags(slot);
		// shell.vert.glsl: v_near = flags != 0u ? 1u : 0u;
		CHECK(flags != 0u);
		CHECK((flags & 1u) == 1u);
		// shell.vert.glsl: uint island = flags >> 8; int i = int(island) - 1;
		const int island = int(flags >> 8);
		const int back = island - 1;
		CHECK(island == slot + 1);
		CHECK(back == slot);
		// The two fields are disjoint, so the near bit can never be read as part of the slot.
		CHECK(island <= 32);
	}
	// A terrain page is uploaded with flags 0 (LodPool::upload), and that is the whole of the
	// shader's "not an island" test -- so no slot may encode to a word a terrain page produces,
	// and no slot may encode to bits 8.. == 0, which the shader reads as "terrain".
	const uint32_t terrain = 0u;
	CHECK(terrain != ve::island_shell_flags(0));
	CHECK(terrain != ve::island_shell_flags(31));
	const uint32_t first = ve::island_shell_flags(0) >> 8;
	CHECK(first == 1u);
}
