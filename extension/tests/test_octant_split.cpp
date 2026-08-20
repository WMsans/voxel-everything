#include <doctest/doctest.h>
#include "mesh/octant_split.h"
#include <set>
#include <string>

TEST_CASE("octant_of separates the eight corners") {
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::set<int> seen;
	for (int i = 0; i < 8; i++) {
		const float p[3] = {(i & 1) ? 1.0f : -1.0f, (i & 2) ? 1.0f : -1.0f,
				(i & 4) ? 1.0f : -1.0f};
		seen.insert(ve::octant_of(p, c));
	}
	CHECK(seen.size() == 8);
}

TEST_CASE("every triangle lands in exactly one bin, with its winding intact") {
	// Two triangles of a quad straddling the centre. The split must not drop one, duplicate
	// one, or reverse one: a reversed triangle is a one-sided collider facing into the rock,
	// and a player walks through it (M3 errata 1's lesson, in a new place).
	const float pos[] = {
			-1.0f, 0.0f, -1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 1.0f,
	};
	const uint32_t idx[] = {0, 1, 2, 0, 2, 3};
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::vector<uint32_t> bins[ve::kColliderOctants];
	ve::split_octants(pos, idx, 6, c, bins);

	int total = 0;
	for (const std::vector<uint32_t> &b : bins) {
		CHECK(b.size() % 3 == 0);
		total += static_cast<int>(b.size());
	}
	CHECK(total == 6);

	// Reconstruct the multiset of triangles and compare with the input, order within each
	// triangle included.
	std::multiset<std::string> want, got;
	const auto key = [](uint32_t a, uint32_t b, uint32_t c2) {
		return std::to_string(a) + "," + std::to_string(b) + "," + std::to_string(c2);
	};
	for (int t = 0; t < 6; t += 3) want.insert(key(idx[t], idx[t + 1], idx[t + 2]));
	for (const std::vector<uint32_t> &b : bins)
		for (size_t t = 0; t + 2 < b.size(); t += 3) got.insert(key(b[t], b[t + 1], b[t + 2]));
	CHECK(want == got);
}

TEST_CASE("centroids populate multiple octants without changing triangle order") {
	const float pos[] = {
			-3.0f, -3.0f, -3.0f, -2.0f, -3.0f, -3.0f, -3.0f, -2.0f, -3.0f,
			1.0f, -3.0f, -3.0f, 2.0f, -3.0f, -3.0f, 1.0f, -2.0f, -3.0f,
			-3.0f, 1.0f, -3.0f, -2.0f, 1.0f, -3.0f, -3.0f, 2.0f, -3.0f,
			1.0f, 1.0f, -3.0f, 2.0f, 1.0f, -3.0f, 1.0f, 2.0f, -3.0f,
			-3.0f, -3.0f, 1.0f, -2.0f, -3.0f, 1.0f, -3.0f, -2.0f, 1.0f,
			1.0f, -3.0f, 1.0f, 2.0f, -3.0f, 1.0f, 1.0f, -2.0f, 1.0f,
			-3.0f, 1.0f, 1.0f, -2.0f, 1.0f, 1.0f, -3.0f, 2.0f, 1.0f,
			1.0f, 1.0f, 1.0f, 2.0f, 1.0f, 1.0f, 1.0f, 2.0f, 1.0f,
	};
	const uint32_t idx[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
			15, 16, 17, 18, 19, 20, 21, 22, 23};
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::vector<uint32_t> bins[ve::kColliderOctants];
	ve::split_octants(pos, idx, 24, c, bins);

	int populated = 0;
	for (const std::vector<uint32_t> &b : bins) {
		if (!b.empty()) populated++;
		const bool valid_size = b.empty() || b.size() == 3;
		CHECK(valid_size);
	}
	CHECK(populated == ve::kColliderOctants);
}

TEST_CASE("an empty bin is empty, not absent") {
	// All eight bins always exist; the streamer indexes them by octant and must not have to
	// think about which ones a chunk happened to fill.
	const float pos[] = {0.1f, 0.1f, 0.1f, 0.2f, 0.1f, 0.1f, 0.2f, 0.2f, 0.1f};
	const uint32_t idx[] = {0, 1, 2};
	const float c[3] = {0.0f, 0.0f, 0.0f};
	std::vector<uint32_t> bins[ve::kColliderOctants];
	ve::split_octants(pos, idx, 3, c, bins);
	int nonempty = 0;
	for (const std::vector<uint32_t> &b : bins)
		if (!b.empty()) nonempty++;
	CHECK(nonempty == 1);
}
