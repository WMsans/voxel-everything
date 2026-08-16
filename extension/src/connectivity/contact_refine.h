#pragma once
#include "connectivity/flood_fill.h"
#include "generator/edit_ops.h"
#include "generator/generator.h"
#include <vector>

namespace ve {

// How the refinement asks whether two cells really touch. An interface for the same reason
// ve::ChunkProbe is one: the real implementation needs the generator, the edit log and the
// volume store, and the edit log's lock lives on the Godot side of the wall.
struct ContactProbe {
	virtual ~ContactProbe() = default;
	// Solid samples on the 0.8 m face between `cell` and `cell + e_axis`, of face_samples^2.
	virtual int contact_samples(IVec3 cell, int axis) const = 0;
};

struct ContactRefineConfig {
	// 9x9 over a 0.8 m face is an 8.9 cm pitch -- spec §5's "true 5 cm SDF along the contact
	// plane" at a density that costs 81 evaluations instead of 256.
	int face_samples = 9;
	// Below this many solid samples the link is severed. 8 of 81 is ~10% of the face, about
	// 0.064 m^2: a hand-sized bridge of rock, which is what "thin" has to mean if a stone
	// arch is to survive and a shattered ledge is not.
	int min_contact_samples = 8;
	// Cutting a link can expose the next one up a chain (a stalk of single cells is a chain
	// of bridges). Three passes covers the shapes the demo tools produce.
	int max_iterations = 3;
	// A bridge that separates more cells than this is the world hanging off the piece, not
	// the piece hanging off the world: never a candidate.
	int max_piece_cells = 512;
	// Field evaluations are the expensive part; cap how many links one pass may test.
	// Candidates are sorted by piece size ascending, so the most island-like go first.
	int max_candidates = 64;
};

// A link whose removal would separate `piece_cells` cells from every shell seed.
struct BridgeLink {
	IVec3 cell{}; // the LOWER cell of the link
	int axis = 0;
	int piece_cells = 0;
};

// Bridges of the anchored subgraph, DFS-rooted at the shell seeds, filtered by
// cfg.max_piece_cells and capped at cfg.max_candidates, smallest piece first.
// Iterative Tarjan: the window holds up to 2 M cells and recursion would overflow the stack.
void find_anchor_bridges(const FloodResult &r, const ContactRefineConfig &cfg,
		std::vector<BridgeLink> *out);

// Finds bridges, asks the probe about each, severs the thin ones and re-floods; repeats
// while cuts are still being made, up to cfg.max_iterations. Returns the number of links
// cut, and leaves `r` re-flooded so the caller can label islands from it directly.
int refine_anchoring(const OccupancyGrid &grid, const ContactProbe &probe,
		const ContactRefineConfig &cfg, LinkCuts *cuts, FloodResult *r);

// The probe's arithmetic, as a pure function: samples the field on the shared face between
// `cell` and `cell + e_axis` on a face_samples^2 lattice inset half a step from the edges,
// and counts how many are solid. This is what the Godot-side probe calls once it has the
// region's op list under the edit lock.
int contact_samples_field(const Generator &gen, const EditOp *ops, int op_count, IVec3 cell,
		int axis, int face_samples, const VolumeStore *volumes = nullptr);

} // namespace ve
