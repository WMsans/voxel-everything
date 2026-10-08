#pragma once
// The sector heightmap tier's geometry (docs/superpowers/specs/2026-10-07-fjords-terrain-design.md
// §4). Mirrored as GLSL constants and functions in shaders/sector.glslh; a sector baked on the
// GPU and sampled here must land on the same texels.
//
// Sector coordinates are FIELD space -- after the world seed's domain shift -- because that is
// the space every stage samples in.
#include <cstdint>
#include <vector>

namespace ve {

inline constexpr int kSectorTexels = 256;
inline constexpr int kSectorApron = 2;
inline constexpr int kSectorStride = kSectorTexels + 2 * kSectorApron; // 260
inline constexpr float kSectorTexelM = 1.6f;
inline constexpr float kSectorSizeM = kSectorTexels * kSectorTexelM;   // 409.6
inline constexpr int kSectorWindow = 24;
// Channel R encodes height over [water_y - below, water_y - below + span].
inline constexpr float kSectorHeightBelowM = 64.0f;
inline constexpr float kSectorHeightSpanM = 512.0f;
// The steepest per-axis texel slope the fjord field stage's declared bound assumes:
// sqrt(1 + 2 * 2.5^2) = 3.67 (spec §6.4).
inline constexpr float kSectorSlopeLimit = 2.5f;

struct SectorCoord {
	int x = 0, z = 0;
	bool operator==(const SectorCoord &o) const { return x == o.x && z == o.z; }
	bool operator!=(const SectorCoord &o) const { return !(*this == o); }
	bool operator<(const SectorCoord &o) const { return z != o.z ? z < o.z : x < o.x; }
};

SectorCoord sector_of(float x, float z);
// Field-space centre of texel (tx, tz), each in [0, kSectorStride); the apron sits outside.
void sector_texel_pos(SectorCoord c, int tx, int tz, float *x, float *z);
// Distance from (x, z) to the sector's square; 0 inside.
float sector_distance(SectorCoord c, float x, float z);

// One baked sector: kSectorStride^2 texels, each packUnorm2x16(r, g) -- r in the low half.
struct SectorTexels {
	std::vector<uint32_t> texels;
	float max_slope = 0.0f; // sector_max_axis_slope(*this), filled on arrival
};

struct SectorSample {
	float r = 0.0f, drdx = 0.0f, drdz = 0.0f, g = 0.0f; // gradient per metre, in r units
};
// Uniform cubic B-spline over the 4x4 texels around field-space (x, z), which lies in c.
SectorSample sector_bspline(const SectorTexels &t, SectorCoord c, float x, float z);
// Largest |r step| along either axis, as metres of height per metre.
float sector_max_axis_slope(const SectorTexels &t);

// The toroidal window the GPU reads through SectorMap.slot[]: kSectorWindow^2 cells of
// (x, z, layer). Only a sector within kSectorWindow / 2 - 1 of `centre` on both axes is
// written, so no two written sectors share a cell; every other cell is
// (INT32_MIN, INT32_MIN, -1).
int sector_window_cell(SectorCoord c);
struct SectorLayer {
	SectorCoord c;
	int layer = -1;
};
std::vector<int32_t> sector_window(SectorCoord centre, const std::vector<SectorLayer> &layers);
int sector_window_lookup(const std::vector<int32_t> &window, SectorCoord c); // -1 when absent

} // namespace ve
