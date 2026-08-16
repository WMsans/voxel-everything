#include "mesh/box_merge.h"
#include <algorithm>
#include <map>
#include <tuple>

namespace ve {

void CellBox::world_aabb(float lo_m[3], float hi_m[3]) const {
	lo_m[0] = static_cast<float>(lo.x) * kOccupancyCellSize;
	lo_m[1] = static_cast<float>(lo.y) * kOccupancyCellSize;
	lo_m[2] = static_cast<float>(lo.z) * kOccupancyCellSize;
	hi_m[0] = static_cast<float>(hi.x + 1) * kOccupancyCellSize;
	hi_m[1] = static_cast<float>(hi.y + 1) * kOccupancyCellSize;
	hi_m[2] = static_cast<float>(hi.z + 1) * kOccupancyCellSize;
}

bool greedy_box_merge(const std::vector<IVec3> &cells, int max_boxes,
		std::vector<CellBox> *out) {
	out->clear();
	if (cells.empty()) return true;

	// A map rather than a dense grid: a component is at most a few hundred cells and may sit
	// anywhere in a 4 km world, so the dense array would be the expensive representation.
	// The value is the "consumed" flag.
	std::map<std::tuple<int, int, int>, char> live;
	for (const IVec3 &c : cells) live[{c.x, c.y, c.z}] = 0;

	const auto free_at = [&live](int x, int y, int z) {
		const auto it = live.find({x, y, z});
		return it != live.end() && it->second == 0;
	};

	// std::map's ordering on the tuple is (x, y, z) lexicographic; iterate explicitly in
	// z, y, x so growth along +x is always into cells the sweep has not reached yet.
	std::vector<IVec3> order;
	order.reserve(live.size());
	for (const auto &kv : live)
		order.push_back({std::get<0>(kv.first), std::get<1>(kv.first), std::get<2>(kv.first)});
	std::sort(order.begin(), order.end(), [](const IVec3 &a, const IVec3 &b) {
		if (a.z != b.z) return a.z < b.z;
		if (a.y != b.y) return a.y < b.y;
		return a.x < b.x;
	});

	for (const IVec3 &seed : order) {
		if (!free_at(seed.x, seed.y, seed.z)) continue;

		int x1 = seed.x;
		while (free_at(x1 + 1, seed.y, seed.z)) x1++;

		int y1 = seed.y;
		for (;;) {
			bool row = true;
			for (int x = seed.x; x <= x1 && row; x++) row = free_at(x, y1 + 1, seed.z);
			if (!row) break;
			y1++;
		}

		int z1 = seed.z;
		for (;;) {
			bool slab = true;
			for (int y = seed.y; y <= y1 && slab; y++)
				for (int x = seed.x; x <= x1 && slab; x++) slab = free_at(x, y, z1 + 1);
			if (!slab) break;
			z1++;
		}

		for (int z = seed.z; z <= z1; z++)
			for (int y = seed.y; y <= y1; y++)
				for (int x = seed.x; x <= x1; x++) live[{x, y, z}] = 1;

		if (static_cast<int>(out->size()) >= max_boxes) {
			out->clear();
			return false;
		}
		out->push_back(CellBox{seed, {x1, y1, z1}});
	}
	return true;
}

} // namespace ve
