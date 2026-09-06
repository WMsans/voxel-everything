#include "shade/sun_cascades.h"

#include "lod/lod_grid.h" // kLodBaseCell, kLodLevels, lod_cell_size
#include <cmath>

namespace ve {

namespace {

// The coarsest level whose CELLS are still at or finer than one shadow texel. Descending
// past it stores detail the map cannot hold.
int min_level_for_texel(float texel_world) {
	int level = 0;
	for (int l = 0; l < kLodLevels; l++) {
		if (lod_cell_size(l) <= texel_world) level = l;
		else break;
	}
	return level;
}

} // namespace

int sun_cascades(float stream_radius_m, int map_size, SunCascade out[kSunCascades]) {
	if (!out) return 0;
	if (!(stream_radius_m > 0.0f) || map_size <= 1) return 0;

	// map_size - 1, not map_size: sun_ortho_sphere's snap moves the min corner DOWN by up
	// to one texel, so the map is a texel wider than the sphere. The texel formula here
	// must match fit_sphere's or the two disagree about what a texel is.
	const float span = float(map_size - 1);
	const float r0 = kLodBaseCell * span * 0.5f;

	// Nothing to split. One cascade at the requested radius IS the pre-cascade behaviour,
	// which is the honest answer rather than three degenerate maps stacked on each other.
	if (stream_radius_m <= r0) {
		out[0].radius = stream_radius_m;
		out[0].texel_world = 2.0f * stream_radius_m / span;
		out[0].min_level = 0;
		return 1;
	}

	out[0].radius = r0;
	out[kSunCascades - 1].radius = stream_radius_m;
	for (int i = 1; i < kSunCascades - 1; i++) {
		// Geometric interpolation between r0 and rN. With kSunCascades == 3 this is the
		// single geometric mean; written as a general step so the constant can move
		// without the formula becoming wrong.
		const double t = double(i) / double(kSunCascades - 1);
		out[i].radius = float(double(r0) *
				std::pow(double(stream_radius_m) / double(r0), t));
	}

	for (int i = 0; i < kSunCascades; i++) {
		out[i].texel_world = 2.0f * out[i].radius / span;
		// Cascade 0 is hard-wired, NOT computed: its texel is kLodBaseCell exactly in real
		// arithmetic, so lod_cell_size(0) <= texel_world is an equality that float rounding
		// can decide either way -- and deciding it wrong silently coarsens every near shadow.
		out[i].min_level = i == 0 ? 0 : min_level_for_texel(out[i].texel_world);
	}
	return kSunCascades;
}

} // namespace ve
