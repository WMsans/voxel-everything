#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "render/camera_params.h"
#include "gpu_layout/blocks.h"
#include "render/gpu/gpu.h"
#include "render/gpu_atlas.h"
#include "render/sun_bake.h"

namespace godot {

// How many slots one frame of the sweep covers. A full lap of the shipping 65 536 slots takes
// 128 frames, about 2 s at 60 fps. The sweep only runs while a lap is owed -- at startup and
// after a sun turn -- because the shipped sun never moves; later slots are covered by the
// per-frame job bake, which re-bakes every brick a frame generated or edited.
inline constexpr int kSunSweepSlotsPerFrame = 512;

// Task 3: bake sun visibility into the brick cache's .w, per voxel, when a brick is generated
// and then again for every resident slot in a rolling sweep. The raymarcher reads the entry
// with one filtered fetch instead of marching a 60 m shadow ray per hit pixel.
//
// Each method records its own compute list; they are called in order before the raymarch and
// the device submits them in that order.
class SunBakePass {
public:
	~SunBakePass();

	bool initialize(RenderingDevice *rd, const GpuAtlas &atlas, RID sun_ubo);
	void teardown();
	bool is_valid() const { return program_.pipeline.is_valid(); }

	// Clears every sun entry when the sun has turned far enough to invalidate them, so a moved
	// sun re-bakes through the sweep instead of shading with the old sun's shadows. A device
	// operation (buffer_clear): call it outside any compute list. Returns true when it cleared,
	// i.e. when the caller owes one full sweep lap.
	bool note_sun(RenderingDevice *rd, const GpuAtlas &atlas, const float dir[3]);

	// This frame's generation jobs, from the job list brick_gen just ran (indirect, one
	// workgroup per job, group count from GpuAtlas::dispatch_args()).
	void bake_jobs(RenderingDevice *rd, const GpuAtlas &atlas, const ve::CameraParams &cam);

	// The rolling re-bake: slots [first_slot, first_slot + count). Skips empty slots on the
	// GPU; the CPU only has to keep the cursor.
	void bake_sweep(RenderingDevice *rd, const GpuAtlas &atlas, const ve::CameraParams &cam,
			int first_slot, int count);

private:
	void record(RenderingDevice *rd, const ve::SunBakePush &push, int groups, RID indirect);

	RenderingDevice *rd_ = nullptr;
	gpu::Group group_;
	gpu::Program program_;
	RID set_;
	RID sun_set_;
	// The direction the current cache was baked under; zeros until the first note_sun, which
	// is what makes the first frame clear (see ve::sun_dir_changed).
	float sun_dir_[3] = {0.0f, 0.0f, 0.0f};
};

} // namespace godot
