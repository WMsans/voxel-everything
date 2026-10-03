// The near-field transparent shell's bookkeeping (docs/superpowers/specs/2026-10-01-
// transparent-voxels-design.md §5, §6). Pure: no godot-cpp, in the native test build.
// LodSystem drives it; LodBuildPass builds the chunks it asks for.
#pragma once
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "lod/lod_contour.h"
#include "lod/lod_grid.h"
#include "world/override_store.h"
#include "world/region.h"
#include <cstdint>
#include <map>
#include <tuple>
#include <vector>

namespace ve {

inline constexpr float kShellCell = 0.1f;
inline constexpr float kShellChunkSize = kShellCell * kLodChunkCells; // 3.2 m
inline constexpr int kShellLevel = -1; // LodBuildJob/Result::level of a near-shell build

void shell_chunk_origin(IVec3 c, float out[3]);
void shell_chunk_aabb(IVec3 c, float lo[3], float hi[3]);
IVec3 shell_chunk_of_point(float x, float y, float z);

struct ShellBox { float lo[3]; float hi[3]; };
// World boxes that may hold transparent solid, among `ops` (already filtered to the area
// of interest), their stored volumes, and the override bricks intersecting [lo, hi].
void transparent_boxes(const EditOp *ops, int op_count, const VolumeSet *volumes,
		const OverrideStore *overrides, const float lo[3], const float hi[3],
		std::vector<ShellBox> *out);
// Chunks overlapping any box (padded by one cell) and within `radius_m` of `cam`.
void shell_candidates(const std::vector<ShellBox> &boxes, const float cam[3], float radius_m,
		std::vector<IVec3> *out);

enum ShellState : uint8_t { kShellUnknown, kShellBuilding, kShellReady, kShellEmpty };

class ShellGrid {
public:
	// Replaces the candidate set. Chunks that left it are appended to `evicted`.
	void set_candidates(const std::vector<IVec3> &chunks, std::vector<IVec3> *evicted);
	void mark_dirty(const float lo[3], const float hi[3]);
	// Up to `max` chunks that need a build (unknown, or dirty and not building), nearest
	// `cam` first.
	void requests(const float cam[3], int max, std::vector<IVec3> *out) const;
	void note_building(IVec3 c);               // clears dirty
	void note_result(IVec3 c, bool has_quads); // Ready or Empty; a dirty chunk stays requestable
	void note_failed(IVec3 c);                 // back to Unknown
	ShellState state(IVec3 c) const;           // kShellUnknown for a chunk not in the set
	bool dirty(IVec3 c) const;
	int size() const;
	void clear();

private:
	struct Node {
		ShellState state = kShellUnknown;
		bool dirty = false;
	};
	using Key = std::tuple<int, int, int>; // (z, y, x): deterministic iteration
	static Key key(IVec3 c) { return Key{c.z, c.y, c.x}; }
	std::map<Key, Node> nodes_;
};

struct IslandShellBlock {
	float origin_local[3]; // chunk origin in the island's LOCAL space
	std::vector<LodQuad> quads;
	std::vector<LodQuadNormals> normals;
};
bool volume_has_transparent(const VolumeData &v);
// Shell quads of an island lattice, split into 32-cell blocks. `lattice_origin` and `voxel`
// are the island descriptor's. Blocks with no quads are omitted.
void island_shell_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		std::vector<IslandShellBlock> *out);

// The chunk-record FLAGS word of an island shell page, exactly as shaders/shell.vert.glsl
// decodes it: `uint flags = floatBitsToUint(chunks.v[ci * 2u + 1u].y)`, then
// `v_near = flags != 0u ? 1u : 0u` and `uint island = flags >> 8` with `i = int(island) - 1`.
// Bit 0 is the near/shell bit; bits 8.. carry atlas_slot + 1, so 0 there means "not an island"
// -- which is why the slot is stored plus one. Named rather than written at the call site
// because its only reader is a shader: nothing else could catch a change to this layout.
inline uint32_t island_shell_flags(int atlas_slot) {
	return 1u | (static_cast<uint32_t>(atlas_slot + 1) << 8);
}

// CPU reference for the thickness the shell passes measure (spec §6). r = sum of back-face
// distances minus front-face distances; g = fronts minus backs; z_opaque <= 0 means sky.
float shell_thickness(float r, float g, float z_opaque, float z_front, float sky_thickness_m);

} // namespace ve
