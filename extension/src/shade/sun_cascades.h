#pragma once

namespace ve {

// Three nested camera-centred sphere fits. Fixed, not configurable: the radii DERIVE from
// kLodBaseCell and the stream radius (see sun_cascades below), and a fourth cascade would
// need a reason that derivation does not supply.
inline constexpr int kSunCascades = 3;

struct SunCascade {
	float radius = 0.0f;      // metres from the camera; sun_ortho_sphere fits this
	float texel_world = 0.0f; // one shadow texel in world metres: 2 * radius / (map_size - 1)
	// The level the shadow cut stops descending at. Resolving geometry finer than one
	// shadow texel is work whose result cannot be stored; see LodTree::shadow_cut.
	int min_level = 0;
};

// Fills `out[0 .. count-1]` innermost-first and returns the count.
//
// The derivation, in full:
//
//   r0 = kLodBaseCell * (map_size - 1) / 2   so cascade 0's texel IS the finest LoD cell.
//                                            Resolving a shadow finer than the finest
//                                            geometry that can cast it buys nothing.
//   rN = stream_radius_m                     so the outermost cascade is EXACTLY the map
//                                            that shipped before cascades existed -- the
//                                            same sun_ortho_sphere call, same arguments.
//   r1 = sqrt(r0 * rN)                       the geometric mean, so texel size steps by a
//                                            constant ratio instead of a chosen constant.
//
// Returns 1 (not kSunCascades) when stream_radius_m <= r0: there is nothing to split, and
// a single cascade is precisely the old behaviour. Returns 0 when the inputs are unusable,
// so a caller can refuse rather than bind a degenerate matrix.
int sun_cascades(float stream_radius_m, int map_size, SunCascade out[kSunCascades]);

} // namespace ve
