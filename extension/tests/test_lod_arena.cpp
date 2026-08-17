#include <doctest/doctest.h>
#include "lod/lod_arena.h"
#include "lod/lod_contour.h"
#include <algorithm>
#include <set>
#include <vector>

TEST_CASE("pages needed rounds up and honours the cap") {
	CHECK(ve::lod_pages_for_quads(0) == 0);
	CHECK(ve::lod_pages_for_quads(1) == 1);
	CHECK(ve::lod_pages_for_quads(ve::kLodQuadsPerPage) == 1);
	CHECK(ve::lod_pages_for_quads(ve::kLodQuadsPerPage + 1) == 2);
	CHECK(ve::lod_pages_for_quads(ve::kLodMaxQuadsPerChunk) == ve::kLodMaxPagesPerChunk);
	CHECK(ve::lod_pages_for_quads(ve::kLodMaxQuadsPerChunk * 4) == ve::kLodMaxPagesPerChunk);
}

TEST_CASE("allocations are distinct and accounted") {
	ve::LodArena a(8);
	CHECK(a.capacity() == 8);
	CHECK(a.free_pages() == 8);
	std::vector<int> p1, p2;
	CHECK(a.alloc(3, &p1));
	CHECK(p1.size() == 3);
	CHECK(a.free_pages() == 5);
	CHECK(a.used_pages() == 3);
	CHECK(a.alloc(5, &p2));
	CHECK(a.free_pages() == 0);
	std::set<int> all(p1.begin(), p1.end());
	all.insert(p2.begin(), p2.end());
	CHECK(all.size() == 8);
	for (int v : all) { CHECK(v >= 0); CHECK(v < 8); }
}

// M3 errata 5's rule, restated for pages: an allocation that cannot be fully funded takes
// NOTHING. A half-allocated chunk would be a hole with pages spent on it.
TEST_CASE("an unfundable allocation takes nothing") {
	ve::LodArena a(4);
	std::vector<int> p;
	CHECK(a.alloc(3, &p));
	CHECK(a.free_pages() == 1);
	std::vector<int> q{99, 98};
	CHECK(a.alloc(2, &q) == false);
	CHECK(q.empty());
	CHECK(a.free_pages() == 1);
	// The one remaining page is still allocatable.
	CHECK(a.alloc(1, &q));
	CHECK(q.size() == 1);
	CHECK(a.free_pages() == 0);
}

TEST_CASE("released pages come back and can be reused") {
	ve::LodArena a(4);
	std::vector<int> p;
	REQUIRE(a.alloc(4, &p));
	CHECK(a.free_pages() == 0);
	a.release(p);
	CHECK(a.free_pages() == 4);
	CHECK(a.used_pages() == 0);
	std::vector<int> q;
	CHECK(a.alloc(4, &q));
	std::sort(p.begin(), p.end());
	std::sort(q.begin(), q.end());
	CHECK(p == q);
}

// A double release would hand the same page to two chunks, which draws one chunk's geometry
// with another's origin. It must be inert, not corrupting.
TEST_CASE("a double release is inert") {
	ve::LodArena a(4);
	std::vector<int> p;
	REQUIRE(a.alloc(2, &p));
	a.release(p);
	CHECK(a.free_pages() == 4);
	a.release(p);
	CHECK(a.free_pages() == 4);
	std::vector<int> q;
	CHECK(a.alloc(4, &q));
	std::set<int> uniq(q.begin(), q.end());
	CHECK(uniq.size() == 4);
}

TEST_CASE("out of range releases are ignored") {
	ve::LodArena a(2);
	std::vector<int> bad{-1, 7, 200};
	a.release(bad);
	CHECK(a.free_pages() == 2);
}

TEST_CASE("clear returns every page") {
	ve::LodArena a(16);
	std::vector<int> p;
	REQUIRE(a.alloc(10, &p));
	a.clear();
	CHECK(a.free_pages() == 16);
	CHECK(a.used_pages() == 0);
}
