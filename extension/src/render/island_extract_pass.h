#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "connectivity/occupancy.h" // ve::CellBox
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "mesh/box_merge.h"         // ve::kMaxIslandBoxes
#include "world/field_source_snapshot.h"

namespace godot {

class VolumePool;
class OverridePool;

// A component's extraction, or a sleeping island's rest-pose resample. One queue, because
// the second is pure CPU (ve::resample_volume) and belongs on the same off-frame thread the
// first already owns -- 262 144 trilinear samples is ~5 ms, which is a hitch on the main
// thread and nothing at all on the worker.
enum IslandJobKind {
	kExtractField = 0,   // evaluate G + ops, masked by `boxes`
	kResampleVolume = 1, // transform `source` by (basis, rest_origin) into a world-aligned volume
};

struct IslandExtractJob {
	IslandJobKind kind = kExtractField;
	int id = -1;
	float origin[3] = {0.0f, 0.0f, 0.0f};
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	std::vector<ve::EditOp> ops;
	std::vector<ve::CellBox> boxes;
	ve::FieldSourceSnapshot snapshot;

	// kResampleVolume only.
	ve::VolumeData source;
	ve::EditOp source_op{};
	float basis[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // ROW major (ve::resample_volume's form)
	float rest_origin[3] = {0.0f, 0.0f, 0.0f};
	int out_slot = -1;
	int override_table = -1;
};

struct IslandExtractResult {
	int id = -1;
	IslandJobKind kind = kExtractField;
	ve::VolumeData data;
	ve::EditOp op{}; // kResampleVolume: the kOpVolumeAdd the manager appends
	bool failed = false;
};

// Spec §5 step 2 and §3's dense per-island volume. Synchronous by design: it runs on the
// mesher's worker thread, where a submit/sync costs nothing the frame can see, and one
// extraction (262 144 field evaluations plus a 1 MB readback) is ~1-2 ms.
class IslandExtractPass {
public:
	~IslandExtractPass();

	// `volumes` is the same pool the mesher's field pass binds, so an extraction sees the
	// rubble already pasted into the world.
	bool initialize(RenderingDevice *rd, const VolumePool *volumes);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }
	OverridePool *overrides() { return overrides_; }
	void set_override_pool(OverridePool *pool) { overrides_ = pool; }

	bool extract(const IslandExtractJob &job, IslandExtractResult *out);

private:
	RenderingDevice *rd_ = nullptr;
	RID out_, boxes_, counts_, ops_;
	RID shader_, pipeline_, uset_;
	OverridePool *overrides_ = nullptr;
};

} // namespace godot
