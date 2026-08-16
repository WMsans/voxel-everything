#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "connectivity/occupancy.h" // ve::CellBox
#include "generator/edit_ops.h"
#include "generator/volume_set.h"
#include "mesh/box_merge.h"         // ve::kMaxIslandBoxes

namespace godot {

class VolumePool;

// One component's worth of work. `ops` and `boxes` are owned, because the job crosses onto
// the mesher's worker thread.
struct IslandExtractJob {
	int id = -1; // the caller's handle, echoed back untouched
	float origin[3] = {0.0f, 0.0f, 0.0f};
	float voxel = ve::kIslandVoxelFine;
	int dim = ve::kIslandDim;
	std::vector<ve::EditOp> ops;   // the component's region's op list at extraction time
	std::vector<ve::CellBox> boxes;
};

struct IslandExtractResult {
	int id = -1;
	ve::VolumeData data;
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

	bool extract(const IslandExtractJob &job, IslandExtractResult *out);

private:
	RenderingDevice *rd_ = nullptr;
	RID out_, boxes_, counts_, ops_;
	RID shader_, pipeline_, uset_;
};

} // namespace godot
