#pragma once
#include "world/brick.h"
#include "generator/generator.h"
#include <cstdint>
#include <vector>

namespace ve {

struct Dims { int x, y, z; };

// CPU brick store for one bounded world. M1: single-level dense indirection;
// the spec's region level arrives with streaming in M2.
class WorldData {
public:
	WorldData(int bx, int by, int bz);

	void generate(const Generator &gen);

	bool brick_active(int bx, int by, int bz) const;
	int brick_slot(int bx, int by, int bz) const; // -1 if inactive
	const Brick &brick(int slot) const { return bricks_[slot]; }
	int active_brick_count() const { return static_cast<int>(bricks_.size()); }
	const std::vector<int32_t> &indirection() const { return indirection_; }
	Dims dims() const { return dims_; }

private:
	Dims dims_;
	std::vector<int32_t> indirection_; // brick grid -> slot or -1
	std::vector<Brick> bricks_;
};

} // namespace ve
