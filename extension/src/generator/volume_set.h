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

// One dense volume: a lattice of encoded SDF plus one global material id per sample.
//
// A byte of material rather than spec §3's 2-bit palette index plus a palette: at 64^3 the
// difference is 192 KB on a pool that is nowhere near its ceiling, and it removes the whole
// palette-packing step from the extract shader and from its CPU reference.
struct VolumeData {
	int dim = kIslandDim;
	std::vector<uint8_t> sdf; // dim^3, ve::encode_sdf
	std::vector<uint8_t> mat; // dim^3, 0 = air
	int solid_voxels = 0;     // how many samples read solid; the island's mass comes from it

	bool empty() const { return sdf.empty(); }
	int voxel_count() const { return dim * dim * dim; }
};

// Trilinear SDF and nearest material from a dim^3 lattice placed at `origin` with pitch
// `voxel`. Mirrored exactly by sample_field_volume() in shaders/field.glslh.
//
// OUTSIDE the lattice's own box the function returns the distance TO that box, which is a
// sound lower bound on the distance to anything the volume contains (the contents are inside
// the box). For the union the op performs that can only ever tighten a positive distance,
// never add material, and it keeps sphere tracing conservative. The extraction pads the
// island so its outermost lattice shell is already positive, so the two branches agree at
// the seam.
bool sample_volume_lattice(const uint8_t *sdf, const uint8_t *mat, int dim,
		const float origin[3], float voxel, float x, float y, float z, VolumeSample *out);

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
	void release(int slot); // frees the bytes; refused on a pinned slot
	void store(int slot, VolumeData data);
	const VolumeData *get(int slot) const;
	int live_count() const { return live_; }
	// Bumped on every store(); the GPU mirrors re-upload a slot when their copy is behind.
	int64_t version(int slot) const;

	// Called the moment an EditOp referencing this slot enters the edit log. A pinned slot
	// can never be released or reused, which is what makes the GPU mirrors safe: the shader
	// has no liveness flag and reads whatever bytes the slot holds, so a slot that an op
	// still names must never come back as a different volume. Live island volumes are NOT
	// pinned -- nothing in the field references them until they are pasted at rest.
	void pin(int slot);
	bool pinned(int slot) const;

	bool sample(int slot, float x, float y, float z, const EditOp &op,
			VolumeSample *out) const override;

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
