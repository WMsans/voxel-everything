#pragma once
#include "generator/edit_ops.h"
#include "world/brick.h"
#include "world/region.h"
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace ve {

// The GPU keeps at most this many override tables, one per consolidated region
// (render/override_pool.h aliases it).
inline constexpr int kMaxOverrideTables = 32;
// S3: which region each table belongs to, in the tail of the GPU table buffer:
// [count, then per table x, y, z, valid]. A sample outside its job's region finds its own
// region's table through these (shaders/field_ops.glslh override_table_at).
inline constexpr int kOverrideTableTagBase = kMaxOverrideTables * kRegionBrickCount;
inline constexpr int kOverrideTableTagInts = 1 + 4 * kMaxOverrideTables;

// One brick's baked field: the same 17^3 encoded lattice the atlas stores, plus one byte of
// GLOBAL material id per cell. Overrides replace the generator base; they are not edit ops.
struct OverrideBrick {
	uint8_t sdf[kBrickSdfCount]{};
	uint8_t mat[kBrickVoxelCount]{};
	std::vector<uint16_t> normal_oct; // optional 17^3 oct snorm8, empty until consolidated
};

// How eval_field asks whether a point's BASE field has been baked. The interface keeps storage
// in world/ and leaves generator/ free of ownership and persistence concerns.
struct OverrideSource {
	virtual ~OverrideSource() = default;
	virtual bool sample(float x, float y, float z, Sample *out) const = 0;
	virtual bool sample_gradient(float x, float y, float z, FieldSample *out) const {
		(void)x; (void)y; (void)z; (void)out;
		return false;
	}
};

// Fixed-capacity pool of baked bricks with a brick-coordinate index. There is no eviction:
// dropping an override would silently restore terrain that the stored ops already replaced.
class OverrideStore : public OverrideSource {
public:
	explicit OverrideStore(int capacity);

	int capacity() const { return static_cast<int>(bricks_.size()); }
	int used() const { return static_cast<int>(index_.size()); }
	int acquire(IVec3 brick);            // existing slot, a new one, or -1 when full
	int slot_of(IVec3 brick) const;      // -1 when absent
	void release(IVec3 brick);
	void clear();
	OverrideBrick *data(int slot);
	const OverrideBrick *data(int slot) const;

	// Trilinear SDF reconstruction over the stored lattice, with the containing cell's
	// material. False when the point is in no override brick.
	bool sample(float x, float y, float z, Sample *out) const override;
	bool sample_gradient(float x, float y, float z, FieldSample *out) const override;

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};

	std::vector<OverrideBrick> bricks_;
	std::vector<int> free_;
	std::map<Key, int> index_;
};

// Every brick of `region` that any of `ops` can reach, padded by the SDF lattice range and
// one lattice pitch. Output order is deterministic with x varying fastest.
void plan_consolidation(const EditOp *ops, int op_count, IVec3 region,
		std::vector<IVec3> *bricks);

// The tag tail for a region -> table map. `count` is one past the highest tagged table, so
// the shader's scan stops early; out-of-range tables are ignored.
std::vector<int32_t> override_table_tags(const std::map<std::tuple<int, int, int>, int> &tables);

} // namespace ve
