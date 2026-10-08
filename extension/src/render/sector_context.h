#pragma once
// The render device's half of the sector tier (spec §4): bakes the pipeline's map stage per
// sector into a ring of storage buffers, reads each back asynchronously into the host SectorCache,
// and keeps the render device's SectorMirror in step. Exists only for a pipeline with a map stage.
#include "render/async_readback.h"
#include "render/gpu/gpu.h"
#include "render/sector_mirror.h"
#include "terrain/sector_cache.h"
#include <array>
#include <memory>

namespace godot {

class FieldContextSet;

class SectorContext {
public:
	~SectorContext();
	bool initialize(RenderingDevice *rd, std::shared_ptr<ve::SectorCache> cache);
	void teardown();
	// Once per frame, before anything evaluates the field: harvest landed bakes into the
	// cache, plan around the camera (world space), dispatch this frame's bakes, sync the
	// render mirror. `field` is the render device's set 1, which the bake binds.
	void run_frame(RenderingDevice *rd, float cam_x, float cam_z, const FieldContextSet *field);
	// World-space rectangles. Render: every wanted sector it overlaps is in the render
	// mirror (what brick generation reads). Host: in the cache (what the CPU field and the
	// worker mirror read).
	bool ready_on_render(float min_x, float min_z, float max_x, float max_z) const;
	bool ready_on_host(float min_x, float min_z, float max_x, float max_z) const;
	const SectorMirror &mirror() const { return mirror_; }
	const ve::SectorCache &cache() const { return *cache_; }
	int64_t bakes_dispatched() const { return dispatched_; }
	void set_bakes_per_frame(int n) { bakes_per_frame_ = n; }

private:
	struct Bake {
		RID buffer;
		Ref<AsyncBufferRead> read;
		gpu::SetCache set;
		ve::SectorCoord c;
		bool busy = false;
	};
	static constexpr int kRing = 8;
	static constexpr int64_t kBakeBytes = int64_t(ve::kSectorStride) * ve::kSectorStride * 4;

	RenderingDevice *rd_ = nullptr;
	std::shared_ptr<ve::SectorCache> cache_;
	gpu::Group group_;
	gpu::Program program_;
	RID op_dummy_;
	std::array<Bake, kRing> ring_;
	SectorMirror mirror_;
	int bakes_per_frame_ = 4;
	int64_t dispatched_ = 0;
};

} // namespace godot
