#include "connectivity/occupancy.h"
#include "world/residency.h"

namespace ve {

int OccupancyGrid::cell_index_in_region(IVec3 cell) {
	// floor_mod, not %, for the same reason brick_mark.comp.glsl uses `& 31`: the brick
	// lattice extends below y = 0 and C++'s % would give -1 for -1.
	const int x = floor_mod(cell.x, kRegionBricks);
	const int y = floor_mod(cell.y, kRegionBricks);
	const int z = floor_mod(cell.z, kRegionBricks);
	return x + y * kRegionBricks + z * kRegionBricks * kRegionBricks;
}

uint8_t OccupancyGrid::read_packed(const uint8_t *block, int index) {
	return static_cast<uint8_t>((block[index >> 2] >> ((index & 3) * 2)) & 0x3);
}

void OccupancyGrid::write_packed(uint8_t *block, int index, uint8_t value) {
	const int shift = (index & 3) * 2;
	uint8_t &b = block[index >> 2];
	b = static_cast<uint8_t>((b & ~(0x3 << shift)) | ((value & 0x3) << shift));
}

OccupancyGrid::Block *OccupancyGrid::ensure(IVec3 region) {
	Block &b = blocks_[key(region)];
	if (b.bytes.empty()) b.bytes.assign(kOccupancyBlockBytes, 0);
	return &b;
}

void OccupancyGrid::set_block(IVec3 region, const uint8_t *bytes, int64_t seq) {
	if (!bytes) return;
	Block *b = ensure(region);
	// Reads can arrive out of order (a region marked in consecutive frames has two
	// requests in flight). An older block must not overwrite a newer one, or the grid and
	// block_seq would regress to a picture from before the latest edit.
	if (seq < b->seq) return;
	b->bytes.assign(bytes, bytes + kOccupancyBlockBytes);
	b->seq = seq;
}

void OccupancyGrid::set_cell(IVec3 cell, CellState s, int64_t seq) {
	Block *b = ensure(ve::region_of_brick(cell));
	write_packed(b->bytes.data(), cell_index_in_region(cell), static_cast<uint8_t>(s));
	if (seq > b->seq) b->seq = seq;
}

CellState OccupancyGrid::state(IVec3 cell) const {
	const auto it = blocks_.find(key(ve::region_of_brick(cell)));
	if (it == blocks_.end()) return kCellUnknown;
	return static_cast<CellState>(read_packed(it->second.bytes.data(),
			cell_index_in_region(cell)));
}

bool OccupancyGrid::has_region(IVec3 region) const {
	return blocks_.find(key(region)) != blocks_.end();
}

int64_t OccupancyGrid::block_seq(IVec3 region) const {
	const auto it = blocks_.find(key(region));
	return it == blocks_.end() ? -1 : it->second.seq;
}

void OccupancyGrid::clear() {
	blocks_.clear();
}

int OccupancyGrid::evict_outside(float cx, float cy, float cz, float retention_m) {
	int dropped = 0;
	for (auto it = blocks_.begin(); it != blocks_.end();) {
		const IVec3 r{it->first.x, it->first.y, it->first.z};
		if (RegionResidency::region_distance(r, cx, cy, cz) > retention_m) {
			it = blocks_.erase(it);
			dropped++;
		} else {
			++it;
		}
	}
	return dropped;
}

} // namespace ve
