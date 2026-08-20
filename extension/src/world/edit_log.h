#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
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
	// Parallel to ops(): the global append sequence number of every op in that region list.
	const std::vector<uint64_t> &seqs(IVec3 region) const;
	int op_count(IVec3 region) const { return static_cast<int>(ops(region).size()); }
	int region_count() const { return static_cast<int>(lists_.size()); }
	const WorldBounds &bounds() const { return bounds_; }
	void clear() {
		lists_.clear();
		seqs_.clear();
		next_seq_ = 1;
	}

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
	std::map<Key, std::vector<uint64_t>> seqs_;
	uint64_t next_seq_ = 1;
};

// Collects every region op that can influence a world-space AABB, for use when an extraction
// or differential probe evaluates one component. A component can straddle a region boundary
// even when it is smaller than a region, so the caller cannot assume one region's op list is
// enough. The collector uses kLatticeFilterPad for stored-lattice consumers (the append path
// remains on the brick residency pad); it gathers all overlapping regions' lists, keeps only
// ops whose own world AABB touches the queried AABB (plus that pad, so boundary-touching ops are
// not dropped), and emits them in GLOBAL append order.
//
// Sequence numbers let us reconstruct the order across region boundaries: each region list is
// only a subsequence of the global op stream, so region iteration order cannot recover it.
// Every copy of one append op shares the same sequence; distinct byte-identical edits have
// distinct sequences and are therefore preserved as separate ops.
inline void collect_ops_for_aabb(const EditLog &log, const float lo[3], const float hi[3],
		std::vector<EditOp> *out) {
	out->clear();
	if (!lo || !hi || lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return;

	constexpr float kPad = kLatticeFilterPad;
	constexpr float kInvRegion = 1.0f / kRegionSize;
	int rlo_a[3] = {0, 0, 0}, rhi_a[3] = {0, 0, 0};
	for (int a = 0; a < 3; a++) {
		const float qlo = lo[a] - kPad;
		const float qhi = hi[a] + kPad;
		rlo_a[a] = static_cast<int>(std::floor(qlo * kInvRegion));
		rhi_a[a] = static_cast<int>(std::ceil(qhi * kInvRegion)) - 1;
		if (rhi_a[a] < rlo_a[a]) return;
	}
	const ve::IVec3 rlo{rlo_a[0], rlo_a[1], rlo_a[2]};
	const ve::IVec3 rhi{rhi_a[0], rhi_a[1], rhi_a[2]};

	const auto intersects = [&](const EditOp &op) {
		return op_touches_aabb(op, lo, hi, kPad);
	};

	struct SeqOp {
		EditOp op;
		uint64_t seq;
	};
	std::vector<SeqOp> found;
	for (int z = rlo.z; z <= rhi.z; z++)
		for (int y = rlo.y; y <= rhi.y; y++)
			for (int x = rlo.x; x <= rhi.x; x++) {
				const ve::IVec3 region{x, y, z};
				if (!log.bounds().contains_region(region)) continue;
				const std::vector<EditOp> &ops = log.ops(region);
				const std::vector<uint64_t> &seqs = log.seqs(region);
				// Both vectors are kept parallel by append(); defensive size handling keeps
				// the helper safe even if a corrupted log ever desynchronised them.
				const size_t n = std::min(ops.size(), seqs.size());
				for (size_t i = 0; i < n; i++) {
					if (!intersects(ops[i])) continue;
					found.push_back({ops[i], seqs[i]});
				}
			}

	// Sort by the global append sequence, then drop region-list duplicates (same sequence)
	// while keeping distinct byte-identical edits (different sequences).
	std::sort(found.begin(), found.end(),
			[](const SeqOp &a, const SeqOp &b) { return a.seq < b.seq; });
	uint64_t last_seq = 0;
	bool have_last = false;
	for (const SeqOp &s : found) {
		if (have_last && s.seq == last_seq) continue;
		have_last = true;
		last_seq = s.seq;
		out->push_back(s.op);
	}
}

} // namespace ve
