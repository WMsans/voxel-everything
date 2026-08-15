#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>
#include "generator/edit_ops.h"
#include "render/mesh_pass.h"
#include "world/region.h"

namespace godot {

// One chunk of work, owning its op list. MeshJob points at a caller-owned array, which is
// fine inside one call but not across a thread hand-off, so the queue carries the ops.
struct MeshRequest {
	ve::IVec3 chunk{};
	std::vector<ve::EditOp> ops;
};

// The collision mesher, moved off the main thread.
//
// MeshPass drives its own local RenderingDevice: submit(), sync() and buffer_get_data() are
// all blocking, and a batch of two chunks cost 14-17 ms of main-thread stall every frame it
// landed one — measured, and the second largest item in the frame after Jolt's shape build.
// None of that stall is work the main thread has to witness: it is waiting for a GPU that is
// not the renderer's.
//
// A RenderingDevice belongs to the thread that CREATES it (Godot's render-thread guard keys
// off that thread id, which is also why creating one on the main thread while the renderer
// runs threaded yields a device the main thread may not touch). So the device and the
// MeshPass are created, used, and destroyed entirely inside run(), and the main thread only
// ever exchanges plain data with it through the two queues below.
class MeshService {
public:
	~MeshService();

	// Spawns the thread and builds the pass on it. Blocks until that has succeeded or failed,
	// so a caller can report the failure exactly where it used to.
	bool start(const MeshPassConfig &cfg);
	void stop();
	bool is_valid() const { return ready_.load(std::memory_order_acquire); }

	// A batch is accepted only when nothing is queued or running, preserving MeshPass's
	// one-batch-at-a-time contract (the residency's in-flight bookkeeping relies on it).
	bool submit(std::vector<MeshRequest> requests);
	// True while a batch is queued or being meshed.
	bool busy() const { return busy_.load(std::memory_order_acquire); }
	// Moves whatever has finished into `out`. Never blocks on the GPU.
	int collect(std::vector<MeshResult> *out);

	// Runs `fn` on the worker thread and waits for it. The diagnostic entry points
	// (debug_mesh_diff and friends) are inherently synchronous and must touch the pass on the
	// thread that owns its device; this is how they still can.
	bool run_sync(const std::function<void(MeshPass &)> &fn);

	float last_collect_ms() const;

private:
	void run();

	std::thread thread_;
	mutable std::mutex mu_;
	std::condition_variable cv_;      // signals the worker: work, sync task, or stop
	std::condition_variable done_cv_; // signals the caller: startup settled, sync task done

	MeshPassConfig cfg_;
	std::vector<MeshRequest> pending_;  // submitted, not yet picked up
	std::vector<MeshResult> results_;   // meshed, not yet collected
	const std::function<void(MeshPass &)> *sync_fn_ = nullptr;
	bool sync_pending_ = false;
	bool started_ = false;   // startup attempt has settled (ready_ is then meaningful)
	bool stopping_ = false;
	std::atomic<bool> ready_{false};
	std::atomic<bool> busy_{false};
	std::atomic<float> last_collect_ms_{0.0f};
};

} // namespace godot
