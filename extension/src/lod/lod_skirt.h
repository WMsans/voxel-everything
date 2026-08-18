#pragma once
#include "lod/lod_quad.h"
#include <vector>

namespace ve {

// A ratio of 2 bounds the level-boundary mismatch at ONE coarse cell, so two cells of
// curtain covers it with margin. (The engine spec's ratio of 4 would have needed four.)
inline constexpr int kLodSkirtCells = 2;

// Appends a curtain for every quad touching a chunk face, returning how many quads were
// added. A boundary quad is one whose owned edge coordinate sits on the first or last cell
// of any axis -- recoverable from the record alone, so there is no fourth GPU pass and no
// cell-map readback. The curtain hangs kLodSkirtCells along the quad's NEGATIVE normal (into
// the solid), which works on cliffs as well as floors where a straight-down skirt does not,
// and it inherits its parent's material so the crack shows the parent's colour.
//
// Each curtain is emitted TWICE with opposite winding rather than needing a second,
// cull-disabled pipeline: skirts are a small fraction of a chunk's quads.
int lod_append_skirts(std::vector<LodQuad> *quads);

} // namespace ve
