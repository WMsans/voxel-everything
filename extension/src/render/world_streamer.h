#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <mutex>
#include <vector>
#include "generator/generator.h"
#include "render/gpu_atlas.h"
#include "render/brick_gen_pass.h"
#include "render/region_pass.h"
#include "world/edit_log.h"
#include "world/residency.h"

namespace godot {

struct PendingEdit; // defined in voxel_world.h; here only the pointer type is needed

// Drives one frame of world maintenance on the render thread: residency loads/evictions,
// edit fan-out, one compute list holding every mark + the indirect generation dispatch.
// Owns nothing; every pointer is borrowed from VoxelWorld.
class WorldStreamer {
public:
	void initialize(ve::RegionResidency *residency, ve::EditLog *edit_log,
			std::mutex *edit_mutex, std::vector<PendingEdit> *pending, GpuAtlas *atlas,
			RegionPass *region_pass, BrickGenPass *brick_gen);

	// Returns the number of actions taken (loads + evicts + edit-mark jobs). Records ONE
	// compute list; the caller submits. buffer_update calls happen before the list opens.
	int run_frame(RenderingDevice *rd, float cx, float cy, float cz);

	int last_frame_edits() const { return frame_edits_; }
	// Sticky OR of every overflow word this streamer has read. The frame word is cleared as
	// soon as it is acted on, so this is the only place a player-facing HUD can learn that
	// the atlas ever came up short — VoxelWorld's own counter only ticks on the debug
	// stream path, which the running demo never takes.
	uint32_t overflow_seen() const { return overflow_seen_; }
	const float *last_edit_center() const { return last_edit_center_; }
	float last_edit_radius() const { return last_edit_radius_; }
	int last_edit_type() const { return last_edit_type_; }
	int last_edit_material() const { return last_edit_material_; }

private:
	ve::RegionResidency *residency_ = nullptr;
	ve::EditLog *edit_log_ = nullptr;
	std::mutex *edit_mutex_ = nullptr;
	std::vector<PendingEdit> *pending_ = nullptr;
	GpuAtlas *atlas_ = nullptr;
	RegionPass *region_pass_ = nullptr;
	BrickGenPass *brick_gen_ = nullptr;
	int frame_edits_ = 0;
	uint32_t overflow_seen_ = 0;
	// Regions marked last frame (loads + on-screen edit jobs). If the job list overflowed
	// (brick_mark.comp.glsl sets frame.overflow bit 1), the next run_frame re-marks these
	// with force_regen so the dropped bricks are re-enqueued (see run_frame).
	std::vector<ve::IVec3> pending_regen_;
	// Loads started in each of the last kInflightFrames frames. The free-slot count is read
	// back from a buffer the GPU is still working through, so on the render thread (where
	// nothing stalls for a submit) it lags the truth by a few frames. Charging the loads
	// already in flight against it is what keeps the streamer from spending an atlas it has
	// not been told is empty yet — without this the reserve is read as intact right up to
	// the frame it is gone.
	static constexpr int kInflightFrames = 4;
	int inflight_loads_[kInflightFrames] = {0, 0, 0, 0};
	int inflight_head_ = 0;
	// Regions still owed a forced re-mark after the free list ran dry. A dropped brick is
	// invisible and nothing else ever comes back for it, so the world would keep the hole
	// for as long as the region stays resident; this queue is what heals it once slots are
	// available again. Drained a couple of regions per frame — a repair, not a stampede.
	std::vector<ve::IVec3> repair_queue_;
	float last_edit_center_[3] = {0.0f, 0.0f, 0.0f};
	float last_edit_radius_ = 0.0f;
	int last_edit_type_ = 0;
	int last_edit_material_ = 0;
};

} // namespace godot
