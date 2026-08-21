#pragma once
#include "generator/edit_ops.h"
#include "world/brick.h"
#include <cstdint>
#include <vector>

namespace ve {

// Spec §3 stores an island as a "dense per-island texture (AABB at 5 cm, uint8 + palette +
// own min-max mip)". M4 fixes the lattice at 64^3 and picks the pitch from the island's
// extent, which bounds every pool in the engine with one constant instead of a size class
// per island: 3.15 m of reach at 5 cm, 6.3 m at 10 cm, and components wider than that are
// split before they ever get here (ve::kMaxIslandExtentCells).
inline constexpr int kIslandDim = 64;
inline constexpr int kIslandVoxelCount = kIslandDim * kIslandDim * kIslandDim; // 262144
inline constexpr float kIslandVoxelFine = 0.05f;   // = kVoxelSize, spec §3's 5 cm
inline constexpr float kIslandVoxelCoarse = 0.10f; // spec §5's "halved ... for large AABBs"
// 64 slots x 256 KB of SDF + 256 KB of material = 33.5 MB, held on the CPU and mirrored on
// both devices. Spec §5 caps the island texture pool at ~512 MB; this is the number that
// forces an early merge long before that cap could bite.
inline constexpr int kMaxVolumes = 64;

// Margin between the component's AABB and the lattice's outer shell, in voxels. It exists so
// the outermost shell is strictly outside every mask box and therefore reads POSITIVE, which
// is what makes sample_volume_lattice's inside and outside branches agree at the seam.
inline constexpr int kIslandMarginVoxels = 2;
// Voxels per min-max cell along each axis. 8 gives an 8^3 chain per 64^3 volume: 1 KB per
// island, the same shape (and the same inclusive-corner rule) as ve::build_brick_mips.
inline constexpr int kVolumeMipStride = 8;

// One dense volume: a lattice of encoded SDF plus one global material id per sample.
//
// A byte of material rather than spec §3's 2-bit palette index plus a palette: at 64^3 the
// difference is 192 KB on a pool that is nowhere near its ceiling, and it removes the whole
// palette-packing step from the extract shader and from its CPU reference.
struct VolumeData {
	int dim = kIslandDim;
	std::vector<uint8_t> sdf; // dim^3, ve::encode_sdf
	std::vector<uint8_t> mat; // dim^3, 0 = air
	std::vector<uint16_t> normal_oct; // dim^3 oct snorm8, optional
	int solid_voxels = 0;     // how many samples read solid; the island's mass comes from it

	// A volume is usable only when 2 <= dim <= kIslandDim and both lattices are
	// present at exactly dim^3 samples; anything else is inconsistent and treated
	// as empty. kIslandDim caps both the arithmetic and every allocation the pool
	// can perform for one volume.
	bool valid() const {
		if (dim < 2 || dim > kIslandDim) return false;
		const size_t expected = static_cast<size_t>(voxel_count());
		if (sdf.size() != expected || mat.size() != expected) return false;
		if (!normal_oct.empty() && normal_oct.size() != expected) return false;
		return true;
	}
	bool empty() const { return !valid(); }
	bool has_normals() const {
		if (dim < 2 || dim > kIslandDim) return false;
		const size_t expected = static_cast<size_t>(voxel_count());
		return normal_oct.size() == expected;
	}
	// Mirrors valid()'s bounds: a dim outside [2, kIslandDim] has no meaningful
	// lattice count and reports 0 rather than overflowing or aliasing.
	int voxel_count() const {
		if (dim < 2 || dim > kIslandDim) return 0;
		return dim * dim * dim;
	}
};

// Where to put a component's lattice: the finest of kIslandVoxelFine / kIslandVoxelCoarse
// whose usable reach ((dim - 1 - 2 * kIslandMarginVoxels) * pitch) covers the AABB, and an
// origin that centres the AABB inside it. False when even the coarse pitch cannot -- the
// caller splits the component (ve::kMaxIslandExtentCells is chosen so that never happens for
// a labelled component, and volume_set.cpp static_asserts the relationship).
bool plan_island_lattice(const float lo[3], const float hi[3], int dim, float *voxel,
		float origin[3]);

// The CPU reference for shaders/island_extract.comp.glsl (spec §8's differential testing):
// the world field intersected with the union of the component's boxes. The intersection is
// what makes the island exactly the material that left, and the carve exactly those boxes.
//
// `box_aabbs` holds 6 floats per box -- min xyz then max xyz, in world space. Flat floats
// rather than ve::CellBox so generator/ need not depend on mesh/: the extractor does not
// care that the boxes came from occupancy cells, only where they are.
void extract_island_volume(const Generator &gen, const EditOp *ops, int op_count,
		const VolumeStore *volumes, const float origin[3], float voxel, int dim,
		const float *box_aabbs, int box_count, VolumeData *out);

// Spec §3's "own min-max mip". Two bytes (min, max) per kVolumeMipStride^3 cell, INCLUSIVE
// over the cell's corner range so a "no surface" verdict is a sound skip for the trilinear
// reconstruction inside it -- the same soundness argument ve::build_brick_mips rests on.
void build_volume_mip(const VolumeData &v, std::vector<uint8_t> *out);

// Trilinear SDF and nearest material from a dim^3 lattice placed at `origin` with pitch
// `voxel`. Mirrored exactly by sample_field_volume() in shaders/field.glslh.
//
// OUTSIDE the lattice's own box the function returns the distance TO that box, which is a
// sound lower bound on the distance to anything the volume contains (the contents are inside
// the box). For the union the op performs that can only ever tighten a positive distance,
// never add material, and it keeps sphere tracing conservative. The extraction pads the
// island so its outermost lattice shell is already positive, but at the seam the outside
// value is a conservative lower bound on the lattice's own value, not an exact match.
bool sample_volume_lattice(const uint8_t *sdf, const uint8_t *mat, int dim,
		const float origin[3], float voxel, float x, float y, float z, VolumeSample *out);

bool sample_volume_gradient_lattice(const uint8_t *sdf, const uint8_t *mat,
		const uint16_t *normal_oct, int dim, const float origin[3], float voxel,
		float x, float y, float z, FieldSample *out);

// The pool of stored volumes, and the VolumeStore every field evaluation consults. The CPU
// copy is AUTHORITATIVE: the render device, the mesher's worker device and ve::raycast all
// read the same field, and only a CPU-side original can feed all three.
class VolumeSet : public VolumeStore {
public:
	int allocate();         // -1 when the pool is full
	// Claims one SPECIFIC free slot; false when it is already in use. It exists because a
	// volume's slot index is baked into the op that names it, so reloading a saved edit log
	// has to put each volume back where it was rather than wherever allocate() felt like.
	bool reserve(int slot);
	// Frees the bytes. Returns false when the slot is out of range, not in use, or
	// pinned (see pin()); true means the slot is free and its buffers were destroyed.
	bool release(int slot);
	// Stores only a complete, consistent volume (both vectors exactly dim^3
	// samples). Returns false and leaves the slot unchanged on invalid data.
	bool store(int slot, VolumeData data);
	const VolumeData *get(int slot) const;
	int live_count() const { return live_; }
	// Bumped on every mutation that allocates, frees, or replaces a slot's bytes
	// (allocate, reserve, release, store); the GPU mirrors re-upload a slot when their
	// copy is behind.
	int64_t version(int slot) const;

	// Called the moment an EditOp referencing this slot enters the edit log. Pinning
	// requires a stored, non-empty volume in the slot (get(slot) != nullptr): an op may
	// only name a pinned slot, and the manager must not let an op reach the log unless
	// its slot has been stored and then pinned. A pinned slot can never be released or
	// reused, which is what makes the GPU mirrors safe: the shader has no liveness flag
	// and reads whatever bytes the slot holds, so a slot that an op still names must
	// never come back as a different volume. Live island volumes are NOT pinned --
	// nothing in the field references them until they are pasted at rest. Returns false
	// and does nothing when the slot has no stored volume (free, or reserved/allocated
	// but still empty).
	bool pin(int slot);
	// Reverses pin() when the EditOp that was about to name this slot was rejected in
	// every region, so no live op references it. A slot pinned by an op that DID reach
	// the log must stay pinned forever; callers are responsible for only unpinning when
	// they can prove no reference exists.
	bool unpin(int slot);
	bool pinned(int slot) const;

	bool has(int slot) const override;
	bool sample(int slot, float x, float y, float z, const EditOp &op,
			VolumeSample *out) const override;
	bool sample_gradient(int slot, float x, float y, float z, const EditOp &op,
			FieldSample *out) const override;

	static int voxel_index(int dim, int x, int y, int z) {
		return x + y * dim + z * dim * dim; // x fastest, as everywhere else
	}

private:
	struct Slot {
		VolumeData data;
		bool used = false;
		bool pinned = false;
		int64_t version = 0;
	};
	Slot slots_[kMaxVolumes];
	int live_ = 0;
	int64_t next_version_ = 1;
};

// Resample `src` -- stored in the body's LOCAL frame at src_op's origin/pitch/dim -- through
// the rigid transform (`basis` row-major 3x3 orthonormal, `origin` translation) into a fresh
// WORLD-AXIS-ALIGNED volume of `dim` samples, and fill in `out_op` as a kOpVolumeAdd for
// `slot`. This is spec §5's "island SDF sampled at rest pose".
//
// The pitch is the finest of kIslandVoxelFine / kIslandVoxelCoarse whose (dim - 1) * pitch
// covers the rotated AABB. Returns false when even the coarse pitch cannot -- the caller
// then falls back to box-add ops and logs (see the plan's Deliberate Deferrals).
bool resample_volume(const VolumeData &src, const EditOp &src_op, const float basis[9],
		const float origin[3], int slot, int dim, VolumeData *out, EditOp *out_op);

} // namespace ve
