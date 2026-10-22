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
// A near-shell chunk is exactly a finest-level LoD chunk (32 cells of 0.1 m), so a shell job
// carries that level: MeshService places its override-table lookup with lod_chunk_origin().
inline constexpr int kShellLevel = kLodMinLevel; // LodBuildJob/Result::level of a near-shell build
static_assert(kShellCell == kLodBaseCell * 0.25f, "the shell grid is LoD level kLodMinLevel");

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
// A terrain-water candidate source: the plane y = level spanning [cam - radius, cam + radius]
// in x and z. shell_candidates' one-cell padding turns the on-boundary plane into the two
// chunk layers straddling level.
ShellBox liquid_plane_box(const float cam[3], float radius_m, float level);

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
	// The shell raster's cover (shaders/shell_cover.glslh): words [x0, y0, z0, nx, ny, nz, 0, 0],
	// then one bit per chunk of the box, bit (x - x0) + nx * ((y - y0) + ny * (z - z0)). The box
	// is the candidates' bounds clipped to [lo, hi] (all zero with none in it). A bit is set once
	// the chunk has had a build result -- a rebuild keeps its old pages drawing, so it stays
	// set. Unset, the far shell keeps the chunk whole: terrain water meshes slower than the far
	// field fades it out.
	void cover(IVec3 lo, IVec3 hi, std::vector<uint32_t> *out) const;
	bool dirty(IVec3 c) const;
	int size() const;
	void clear();

private:
	struct Node {
		ShellState state = kShellUnknown;
		bool dirty = false;
		bool built = false; // some build result landed; never cleared while a candidate
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
// Which mesh of an island lattice to contour. kShell: the transparent shell, only quads whose
// solid side is transparent (empty when the volume holds no transparent label). kOpaque: the
// opaque view (a transparent solid reads as outside), every quad -- raster mode draws the
// island from it instead of marching it (spec 2026-10-04 §5).
enum class IslandMeshKind { kShell, kOpaque };
// That mesh, split into 32-cell blocks. `lattice_origin` and `voxel` are the island
// descriptor's. Blocks with no quads are omitted.
void island_blocks(const VolumeData &v, const float lattice_origin[3], float voxel,
		IslandMeshKind kind, std::vector<IslandShellBlock> *out);

// The chunk-record FLAGS word of an island shell page, exactly as shaders/shell.vert.glsl
// decodes it: `uint flags = floatBitsToUint(chunks.v[ci * 2u + 1u].y)`, then
// `v_near = flags != 0u ? 1u : 0u` and `uint island = flags >> 8` with `i = int(island) - 1`.
// Bit 0 is the near/shell bit; bits 8.. carry atlas_slot + 1, so 0 there means "not an island"
// -- which is why the slot is stored plus one. Named rather than written at the call site
// because its only reader is a shader: nothing else could catch a change to this layout.
inline uint32_t island_shell_flags(int atlas_slot) {
	return 1u | (static_cast<uint32_t>(atlas_slot + 1) << 8);
}

// The chunk-record FLAGS word of an island's OPAQUE mesh page: the slot in bits 8.. exactly
// as island_shell_flags, with bit 0 clear because it is not a shell page. lod.vert.glsl
// places it through the island descriptor.
inline uint32_t island_mesh_flags(int atlas_slot) {
	return static_cast<uint32_t>(atlas_slot + 1) << 8;
}

// CPU reference for the thickness the shell passes measure (spec §6). r = sum of back-face
// distances minus front-face distances; g = fronts minus backs; z_opaque <= 0 means sky.
float shell_thickness(float r, float g, float z_opaque, float z_front, float sky_thickness_m);

} // namespace ve
