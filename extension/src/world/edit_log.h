#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

// Spec §2 caps a region at 256 ops before consolidation into override bricks. M2 has no
// consolidation (see the plan's Deliberate Deferrals): a full region rejects the op and the
// caller logs it — spec §8's fail-soft rule, "warn + no-op".
inline constexpr int kMaxRegionOps = 256;

// The ordered CSG op lists that turn G into the current world (spec §2). An op is appended
// to EVERY region it touches, so reconstructing any brick needs only that brick's own
// region list — no neighbour walk, on CPU or GPU.
class EditLog {
public:
	struct AppendResult {
		std::vector<IVec3> touched;  // in-bounds regions whose list grew
		std::vector<IVec3> rejected; // in-bounds regions already holding kMaxRegionOps
	};

	explicit EditLog(const WorldBounds &bounds) : bounds_(bounds) {}

	AppendResult append(const EditOp &op);
	const std::vector<EditOp> &ops(IVec3 region) const;
	int op_count(IVec3 region) const { return static_cast<int>(ops(region).size()); }
	int region_count() const { return static_cast<int>(lists_.size()); }
	const WorldBounds &bounds() const { return bounds_; }
	void clear() { lists_.clear(); }

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};

	WorldBounds bounds_;
	std::map<Key, std::vector<EditOp>> lists_;
};

} // namespace ve
