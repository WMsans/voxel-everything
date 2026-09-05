#pragma once
#include "world/region.h"
#include <cstdint>
#include <map>
#include <vector>

namespace ve {

// Spec §5's "global persistent occupancy grid, 0.8 m cells = one bit per brick". A cell and
// a brick are the same lattice: cell c spans world [c * kBrickSize, (c + 1) * kBrickSize).
inline constexpr float kOccupancyCellSize = kBrickSize; // 0.8 m
// Two bits per cell, four cells per byte: 32768 cells per region -> 8192 bytes.
inline constexpr int kOccupancyBlockBytes = kRegionBrickCount / 4;

// Two bits, because the flood fill and the thin-contact refinement want different questions
// answered and the mark pass already has both answers in registers (its 3^3 probe reduces a
// min and a max over the brick). kCellUnknown is the ZERO state on purpose: a freshly
// allocated or freshly released block reads as "nobody has looked", which is the state the
// grid must never confuse with "air".
enum CellState : uint8_t {
	kCellUnknown = 0, // never probed
	kCellAir = 1,     // probe found no solid sample
	kCellSolid = 2,   // some solid, some air -- a surface crosses the cell
	kCellFull = 3,    // no air sample at all
};

// Which cells hold matter, kept in sparse per-region blocks. Regions arrive from the GPU mark
// pass and are retained while they are near the camera: a region's occupancy is recomputable
// from the mark pass against edits and overrides, so dropping a distant block is lossless, and
// in an unbounded world retaining every block ever probed grows with distance travelled
// (~92 MB per 4 km). A dropped block reads back as kCellUnknown -- "nobody has looked" -- which
// is the state this grid must never confuse with air.
//
// Not thread-safe. It is written and read on the main thread only; the render thread hands
// blocks over through VoxelWorld's occupancy inbox (Task 8).
class OccupancyGrid {
public:
	// `bytes` must hold kOccupancyBlockBytes. `seq` is the world's edit sequence number as
	// of the mark that produced it; IslandManager waits on it before trusting a window.
	void set_block(IVec3 region, const uint8_t *bytes, int64_t seq);
	// One cell, creating an all-unknown block if the region has none. The GPU never uses
	// this; it exists for tests and for the manager's own bookkeeping after a carve.
	void set_cell(IVec3 cell, CellState s, int64_t seq);

	CellState state(IVec3 cell) const;
	// NOT KNOWN TO BE AIR. Unknown counts as solid -- see the plan's Deliberate Decisions.
	bool is_solid(IVec3 cell) const { return state(cell) != kCellAir; }
	bool is_full(IVec3 cell) const { return state(cell) == kCellFull; }
	bool is_known(IVec3 cell) const { return state(cell) != kCellUnknown; }

	bool has_region(IVec3 region) const;
	int64_t block_seq(IVec3 region) const; // -1 when the region has no block
	int region_count() const { return static_cast<int>(blocks_.size()); }
	void clear();

	// Drop every block whose region lies further than retention_m from the point. Returns the
	// number dropped. Main thread only, like every other method here.
	int evict_outside(float cx, float cy, float cz, float retention_m);

	// 0..kRegionBrickCount-1, x fastest then y then z, floor-modulo so negative cells land
	// on the last cell of the region below rather than off the front of the block.
	static int cell_index_in_region(IVec3 cell);
	static uint8_t read_packed(const uint8_t *block, int index);
	static void write_packed(uint8_t *block, int index, uint8_t value);

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	static Key key(IVec3 r) { return Key{r.x, r.y, r.z}; }
	struct Block {
		std::vector<uint8_t> bytes;
		int64_t seq = -1;
	};
	Block *ensure(IVec3 region);

	std::map<Key, Block> blocks_;
};

} // namespace ve
