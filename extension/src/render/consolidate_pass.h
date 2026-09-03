#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "render/override_pool.h"
#include "world/region.h"
#include "world/edit_log.h"
#include "generator/edit_ops.h"
#include "world/field_source_snapshot.h"

namespace godot {

class FieldContextSet;

struct ConsolidateJob {
	ve::IVec3 region{};
	int region_slot = -1;
	uint64_t through_seq = 0;
	std::vector<ve::IVec3> bricks;
	std::vector<ve::EditOp> ops;
	ve::FieldSourceSnapshot source;

	// Borrowed from WorldStore, captured when the job was submitted on the main thread.
	// Never an owned CPU generator: the terrain pipeline can swap the world's field.
	const ve::Generator *gen = nullptr;
};

struct ConsolidateResult {
	ve::IVec3 region{};
	std::vector<ve::IVec3> bricks;
	std::vector<ve::OverrideBrick> baked;
	bool failed = false;
};

class VolumePool;
class ConsolidatePass {
public:
	~ConsolidatePass();
	// max_bricks is bounded by the override pool and controls only the transient staging
	// buffers; staged output never aliases a published override slot.
	bool initialize(RenderingDevice *rd, OverridePool *pool, VolumePool *volumes, int max_bricks = 0);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }
	// The worker device's set 1, owned by MeshService and valid for the worker's whole
	// run. Borrowed, never freed here; bound beside set 0.
	void set_field_context(const FieldContextSet *fc) { field_context_ = fc; }
	bool run(const ConsolidateJob &job, ConsolidateResult *out);

private:
	RenderingDevice *rd_ = nullptr;
	const FieldContextSet *field_context_ = nullptr;
	OverridePool *pool_ = nullptr;
	int max_bricks_ = 0;
	RID shader_, pipeline_, uset_, ops_, jobs_;
	RID staging_sdf_, staging_mat_;
};

} // namespace godot
