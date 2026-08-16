#include "connectivity/contact_refine.h"
#include "world/brick_eval.h"
#include <algorithm>

namespace ve {

namespace {

// Direction d is (axis d/2, sign +/-); d ^ 1 is its reverse, which is how the DFS skips the
// tree edge back to its parent without storing the parent node.
constexpr int kAxis[6] = {0, 0, 1, 1, 2, 2};
constexpr int kSign[6] = {+1, -1, +1, -1, +1, -1};

IVec3 step(IVec3 c, int d) {
	IVec3 n = c;
	if (kAxis[d] == 0) n.x += kSign[d];
	else if (kAxis[d] == 1) n.y += kSign[d];
	else n.z += kSign[d];
	return n;
}

IVec3 link_cell(IVec3 c, int d) { return kSign[d] > 0 ? c : step(c, d); }

struct Frame {
	int node = -1;
	int from = -1;  // the direction this node was entered by, or -1 at a root
	int next = 0;   // the next direction to try
};

} // namespace

void find_anchor_bridges(const FloodResult &r, const ContactRefineConfig &cfg,
		std::vector<BridgeLink> *out) {
	out->clear();
	const FloodWindow &w = r.window;
	const int n = w.cells();
	std::vector<int> disc(static_cast<size_t>(n), -1);
	std::vector<int> low(static_cast<size_t>(n), 0);
	std::vector<int> sub(static_cast<size_t>(n), 0);   // subtree cell count
	std::vector<int> seeds(static_cast<size_t>(n), 0); // shell cells in the subtree
	int timer = 0;

	std::vector<Frame> stack;
	std::vector<BridgeLink> found;
	for (int s = 0; s < n; s++) {
		if (disc[s] >= 0 || !r.solid[s] || !r.anchored[s]) continue;
		if (!w.on_boundary(w.cell_of(s))) continue; // roots are shell seeds only
		disc[s] = low[s] = timer++;
		sub[s] = 1;
		seeds[s] = 1;
		stack.push_back(Frame{s, -1, 0});
		while (!stack.empty()) {
			Frame &f = stack.back();
			if (f.next < 6) {
				const int d = f.next++;
				if (d == (f.from ^ 1)) continue; // the edge back to the parent
				const IVec3 c = w.cell_of(f.node);
				const int ni = w.index(step(c, d));
				if (ni < 0 || !r.solid[ni] || !r.anchored[ni]) continue;
				if (disc[ni] >= 0) {
					low[f.node] = std::min(low[f.node], disc[ni]);
					continue;
				}
				disc[ni] = low[ni] = timer++;
				sub[ni] = 1;
				seeds[ni] = w.on_boundary(w.cell_of(ni)) ? 1 : 0;
				stack.push_back(Frame{ni, d, 0});
				continue;
			}
			const Frame child = stack.back();
			stack.pop_back();
			if (stack.empty()) break;
			const int p = stack.back().node;
			low[p] = std::min(low[p], low[child.node]);
			sub[p] += sub[child.node];
			seeds[p] += seeds[child.node];
			if (low[child.node] > disc[p] && seeds[child.node] == 0) {
				// Removing this edge separates child's subtree from every shell seed.
				const IVec3 pc = w.cell_of(p);
				found.push_back(BridgeLink{link_cell(pc, child.from), kAxis[child.from],
						sub[child.node]});
			}
		}
	}

	found.erase(std::remove_if(found.begin(), found.end(),
						[&cfg](const BridgeLink &b) { return b.piece_cells > cfg.max_piece_cells; }),
			found.end());
	std::sort(found.begin(), found.end(), [](const BridgeLink &a, const BridgeLink &b) {
		return a.piece_cells < b.piece_cells;
	});
	if (static_cast<int>(found.size()) > cfg.max_candidates)
		found.resize(static_cast<size_t>(cfg.max_candidates));
	*out = std::move(found);
}

int refine_anchoring(const OccupancyGrid &grid, const ContactProbe &probe,
		const ContactRefineConfig &cfg, LinkCuts *cuts, FloodResult *r) {
	int total = 0;
	std::vector<BridgeLink> bridges;
	for (int iter = 0; iter < cfg.max_iterations; iter++) {
		find_anchor_bridges(*r, cfg, &bridges);
		int made = 0;
		for (const BridgeLink &b : bridges) {
			if (cuts->cut(b.cell, b.axis)) continue;
			if (probe.contact_samples(b.cell, b.axis) >= cfg.min_contact_samples) continue;
			cuts->add(b.cell, b.axis);
			made++;
		}
		if (made == 0) break;
		total += made;
		flood_anchored(grid, r->window, cuts, r);
	}
	return total;
}

int contact_samples_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		int axis, int face_samples, const VolumeStore *volumes) {
	if (face_samples < 1) return 0;
	// The face is the plane at the far side of `cell` along `axis`; the other two axes span
	// the cell's own extent. Samples are inset half a step so none lands on a corner shared
	// with three other faces, where a hairline of rock would read as contact on all of them.
	const int u = (axis + 1) % 3, v = (axis + 2) % 3;
	const int c[3] = {cell.x, cell.y, cell.z};
	float base[3];
	base[axis] = static_cast<float>(c[axis] + 1) * kOccupancyCellSize;
	base[u] = static_cast<float>(c[u]) * kOccupancyCellSize;
	base[v] = static_cast<float>(c[v]) * kOccupancyCellSize;
	const float step_m = kOccupancyCellSize / static_cast<float>(face_samples);

	int solid = 0;
	for (int j = 0; j < face_samples; j++)
		for (int i = 0; i < face_samples; i++) {
			float p[3] = {base[0], base[1], base[2]};
			p[u] += (static_cast<float>(i) + 0.5f) * step_m;
			p[v] += (static_cast<float>(j) + 0.5f) * step_m;
			if (eval_field(gen, ops, op_count, p[0], p[1], p[2], volumes).sdf <= 0.0f) solid++;
		}
	return solid;
}

} // namespace ve
