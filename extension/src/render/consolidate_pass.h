#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "render/override_pool.h"
#include "world/region.h"
#include "world/edit_log.h"
#include "generator/edit_ops.h"

namespace godot {

struct ConsolidateJob {
	ve::IVec3 region{};
	int region_slot = -1;
	std::vector<ve::IVec3> bricks;
	std::vector<ve::EditOp> ops;
};

struct ConsolidateResult {
	ve::IVec3 region{};
	std::vector<ve::IVec3> bricks;
	std::vector<ve::OverrideBrick> baked;
	bool failed = false;
};

class ConsolidatePass {
public:
	~ConsolidatePass();
	// max_bricks is bounded by the override pool and controls only the transient staging
	// buffers; staged output never aliases a published override slot.
	bool initialize(RenderingDevice *rd, OverridePool *pool, int max_bricks = 0);
	void teardown();
	bool is_valid() const { return pipeline_.is_valid(); }
	bool run(const ConsolidateJob &job, ConsolidateResult *out);

private:
	RenderingDevice *rd_ = nullptr;
	OverridePool *pool_ = nullptr;
	int max_bricks_ = 0;
	RID shader_, pipeline_, uset_, ops_, jobs_;
	RID staging_sdf_, staging_mat_;
};

} // namespace godot
