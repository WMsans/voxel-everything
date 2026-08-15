#include "world/world_data.h"
#include "world/brick_eval.h"

namespace ve {

WorldData::WorldData(int bx, int by, int bz)
	: dims_{bx, by, bz}, indirection_(static_cast<size_t>(bx) * by * bz, -1) {}

void WorldData::generate(const Generator &gen) {
	// M2: WorldData is no longer the renderer's source of truth — it is the CPU reference
	// the GPU differential test diffs against (spec §8), so it must walk exactly the same
	// evaluator the GPU mirrors. No edit ops: WorldData models the unedited base world.
	for (int bz = 0; bz < dims_.z; bz++)
		for (int by = 0; by < dims_.y; by++)
			for (int bx = 0; bx < dims_.x; bx++) {
				const IVec3 brick{bx, by, bz};
				if (!brick_has_surface(gen, nullptr, 0, brick)) continue;
				const int slot = static_cast<int>(bricks_.size());
				indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y] =
						slot;
				BrickEval e{};
				eval_brick(gen, nullptr, 0, brick, &e);
				bricks_.push_back(e.brick);
				mips_.push_back(e.mips);
			}
}

bool WorldData::brick_active(int bx, int by, int bz) const {
	return brick_slot(bx, by, bz) >= 0;
}

int WorldData::brick_slot(int bx, int by, int bz) const {
	if (bx < 0 || by < 0 || bz < 0 || bx >= dims_.x || by >= dims_.y || bz >= dims_.z) return -1;
	return indirection_[static_cast<size_t>(bx) + by * dims_.x + bz * dims_.x * dims_.y];
}

} // namespace ve
