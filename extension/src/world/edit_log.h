#pragma once
#include "generator/edit_ops.h"
#include "world/region.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace ve {

// Spec §2 caps a region at 256 ops before consolidation into override bricks. M2 has no
// consolidation (see the plan's Deliberate Deferrals): a full region rejects the op and the
// caller logs it — spec §8's fail-soft rule, "warn + no-op".
inline constexpr int kMaxRegionOps = 256;
inline constexpr int kConsolidateAtOps = 192;
// L7 is one 1638.4 m chunk (64 regions), and gather_lod_ops pads it by two L7 cells
// before this helper adds the lattice pad. Keep that valid query room while bounding a
// hostile AABB to at most 128^3 region-map lookups; the edit-append guard remains 64.
inline constexpr int kMaxCollectorRegionSpan = 128;

// The ordered CSG op lists that turn G into the current world (spec §2). An op is appended
// to EVERY region it touches, so reconstructing any brick needs only that brick's own
// region list — no neighbour walk, on CPU or GPU.
class EditLog {
public:
	struct AppendResult {
		std::vector<IVec3> touched;  // in-bounds regions whose list grew
		std::vector<IVec3> rejected; // in-bounds regions already holding kMaxRegionOps
		// Set when the op's region span exceeds ve::kMaxOpRegionSpan: the op is refused
		// outright rather than iterated, so touched and rejected stay empty.
		bool oversized = false;
	};

	EditLog() = default;

	AppendResult append(const EditOp &op);
	const std::vector<EditOp> &ops(IVec3 region) const;
	// Parallel to ops(): the global append sequence number of every op in that region list.
	const std::vector<uint64_t> &seqs(IVec3 region) const;
	int op_count(IVec3 region) const { return static_cast<int>(ops(region).size()); }
	void clear_region(IVec3 region);
	// Remove only the prefix captured by an asynchronous consolidation. Edits appended after
	// the bake began remain in the region list and are evaluated against the newly published
	// override on the next bake.
	void clear_region_through(IVec3 region, uint64_t seq);
	int region_count() const { return static_cast<int>(lists_.size()); }
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
	if (!out) return;
	out->clear();
	if (!lo || !hi || lo[0] > hi[0] || lo[1] > hi[1] || lo[2] > hi[2]) return;
	for (int a = 0; a < 3; a++)
		if (!std::isfinite(lo[a]) || !std::isfinite(hi[a])) return;

	constexpr float kPad = kLatticeFilterPad;
	int rlo_a[3] = {0, 0, 0}, rhi_a[3] = {0, 0, 0};
	for (int a = 0; a < 3; a++) {
		const double qlo = static_cast<double>(lo[a]) - kPad;
		const double qhi = static_cast<double>(hi[a]) + kPad;
		const double rlo = std::floor(qlo / static_cast<double>(kRegionSize));
		const double rhi = std::ceil(qhi / static_cast<double>(kRegionSize)) - 1.0;
		if (!std::isfinite(rlo) || !std::isfinite(rhi) ||
				rlo < static_cast<double>(std::numeric_limits<int>::min()) ||
				rhi > static_cast<double>(std::numeric_limits<int>::max()))
			return;
		rlo_a[a] = static_cast<int>(rlo);
		rhi_a[a] = static_cast<int>(rhi);
		if (rhi_a[a] < rlo_a[a]) return;
	}
	const ve::IVec3 rlo{rlo_a[0], rlo_a[1], rlo_a[2]};
	const ve::IVec3 rhi{rhi_a[0], rhi_a[1], rhi_a[2]};
	const auto span = [](int low, int high) -> uint64_t {
		return high < low ? 0u : static_cast<uint64_t>(high) - static_cast<uint64_t>(low) + 1u;
	};
	if (span(rlo.x, rhi.x) > kMaxCollectorRegionSpan ||
			span(rlo.y, rhi.y) > kMaxCollectorRegionSpan ||
			span(rlo.z, rhi.z) > kMaxCollectorRegionSpan)
		return;

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
