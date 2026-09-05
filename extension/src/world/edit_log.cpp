#include "world/edit_log.h"
#include <algorithm>

namespace ve {

namespace {
const std::vector<EditOp> kEmpty;
const std::vector<uint64_t> kEmptySeqs;
} // namespace

EditLog::AppendResult EditLog::append(const EditOp &op) {
	AppendResult result;
	IVec3 lo{}, hi{};
	op_region_range(op, &lo, &hi);
	// The world has no edge, so there is nothing to clamp against. The bound that used to
	// fall out of the box is now stated directly: an op whose region range is absurd is
	// refused outright rather than iterated (ve::kMaxOpRegionSpan).
	if (!op_region_span_ok(op)) {
		result.oversized = true;
		return result;
	}
	// One global sequence per append op. Every region that accepts this op stores the same
	// sequence, so a cross-region collector can reconstruct the true append order.
	const uint64_t seq = next_seq_++;
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++) {
				const IVec3 r{x, y, z};
				const Key key{r.x, r.y, r.z};
				std::vector<EditOp> &list = lists_[key];
				if (static_cast<int>(list.size()) >= kMaxRegionOps) {
					result.rejected.push_back(r);
					continue;
				}
				list.push_back(op);
				seqs_[key].push_back(seq);
				result.touched.push_back(r);
			}
	return result;
}

const std::vector<EditOp> &EditLog::ops(IVec3 region) const {
	const auto it = lists_.find(Key{region.x, region.y, region.z});
	return it == lists_.end() ? kEmpty : it->second;
}

const std::vector<uint64_t> &EditLog::seqs(IVec3 region) const {
	const auto it = seqs_.find(Key{region.x, region.y, region.z});
	return it == seqs_.end() ? kEmptySeqs : it->second;
}

void EditLog::clear_region(IVec3 region) {
	const Key key{region.x, region.y, region.z};
	lists_.erase(key);
	seqs_.erase(key);
}

void EditLog::clear_region_through(IVec3 region, uint64_t seq) {
	const Key key{region.x, region.y, region.z};
	auto ops_it = lists_.find(key);
	auto seqs_it = seqs_.find(key);
	if (ops_it == lists_.end() || seqs_it == seqs_.end()) return;
	std::vector<EditOp> &ops = ops_it->second;
	std::vector<uint64_t> &seqs = seqs_it->second;
	const size_t n = std::min(ops.size(), seqs.size());
	size_t cut = 0;
	while (cut < n && seqs[cut] <= seq) cut++;
	if (cut == 0) return;
	ops.erase(ops.begin(), ops.begin() + static_cast<ptrdiff_t>(cut));
	seqs.erase(seqs.begin(), seqs.begin() + static_cast<ptrdiff_t>(cut));
	if (ops.empty()) lists_.erase(ops_it);
	if (seqs.empty()) seqs_.erase(seqs_it);
}

} // namespace ve
