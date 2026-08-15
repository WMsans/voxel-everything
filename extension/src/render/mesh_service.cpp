#include "render/mesh_service.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

MeshService::~MeshService() {
	stop();
}

bool MeshService::start(const MeshPassConfig &cfg) {
	stop();
	{
		std::lock_guard<std::mutex> lock(mu_);
		cfg_ = cfg;
		started_ = false;
		stopping_ = false;
		ready_.store(false, std::memory_order_release);
		busy_.store(false, std::memory_order_release);
	}
	thread_ = std::thread([this] { run(); });
	std::unique_lock<std::mutex> lock(mu_);
	done_cv_.wait(lock, [this] { return started_; });
	const bool ok = ready_.load(std::memory_order_acquire);
	lock.unlock();
	if (!ok) stop(); // the thread has already exited; just join it
	return ok;
}

void MeshService::stop() {
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (!thread_.joinable() && !stopping_) return;
		stopping_ = true;
	}
	cv_.notify_all();
	if (thread_.joinable()) thread_.join();
	std::lock_guard<std::mutex> lock(mu_);
	stopping_ = false;
	started_ = false;
	pending_.clear();
	results_.clear();
	ready_.store(false, std::memory_order_release);
	busy_.store(false, std::memory_order_release);
}

bool MeshService::submit(std::vector<MeshRequest> requests) {
	if (requests.empty() || !is_valid()) return false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_ || !pending_.empty() || busy_.load(std::memory_order_acquire)) return false;
		pending_ = std::move(requests);
		// Set here, under the lock, rather than when the worker wakes: run_frame asks busy()
		// on the very next line and a batch that is queued but not yet picked up must already
		// count, or the residency would plan the same chunks twice.
		busy_.store(true, std::memory_order_release);
	}
	cv_.notify_one();
	return true;
}

int MeshService::collect(std::vector<MeshResult> *out) {
	std::vector<MeshResult> got;
	{
		std::lock_guard<std::mutex> lock(mu_);
		got.swap(results_);
	}
	const int n = static_cast<int>(got.size());
	if (out)
		for (MeshResult &r : got) out->push_back(std::move(r));
	return n;
}

bool MeshService::run_sync(const std::function<void(MeshPass &)> &fn) {
	if (!is_valid()) return false;
	std::unique_lock<std::mutex> lock(mu_);
	if (stopping_) return false;
	// One at a time, and never while a batch is running: the diagnostic hooks share the
	// pass's single lattice and cell map with the streaming path.
	done_cv_.wait(lock, [this] {
		return stopping_ || (!sync_pending_ && !busy_.load(std::memory_order_acquire));
	});
	if (stopping_) return false;
	sync_fn_ = &fn;
	sync_pending_ = true;
	cv_.notify_one();
	done_cv_.wait(lock, [this] { return !sync_pending_ || stopping_; });
	return !stopping_;
}

float MeshService::last_collect_ms() const {
	return last_collect_ms_.load(std::memory_order_relaxed);
}

void MeshService::run() {
	// Created here on purpose: a RenderingDevice may only be used from the thread that made
	// it (see the class comment), so the device, the pass, and their teardown all live in
	// this function's scope and never escape it.
	RenderingDevice *rd = RenderingServer::get_singleton()->create_local_rendering_device();
	MeshPass pass;
	const bool ok = rd && pass.initialize(rd, cfg_);
	if (!rd) UtilityFunctions::printerr("MeshService: no local RenderingDevice for the mesher");
	{
		std::lock_guard<std::mutex> lock(mu_);
		ready_.store(ok, std::memory_order_release);
		started_ = true;
	}
	done_cv_.notify_all();
	if (!ok) {
		pass.teardown();
		if (rd) memdelete(rd);
		return;
	}

	for (;;) {
		std::vector<MeshRequest> batch;
		const std::function<void(MeshPass &)> *sync_fn = nullptr;
		{
			std::unique_lock<std::mutex> lock(mu_);
			cv_.wait(lock, [this] { return stopping_ || !pending_.empty() || sync_pending_; });
			if (stopping_) break;
			if (sync_pending_) {
				sync_fn = sync_fn_;
			} else {
				batch.swap(pending_);
			}
		}

		if (sync_fn) {
			(*sync_fn)(pass);
			{
				std::lock_guard<std::mutex> lock(mu_);
				sync_pending_ = false;
				sync_fn_ = nullptr;
			}
			done_cv_.notify_all();
			continue;
		}

		// Point the jobs at the ops this batch owns; `batch` outlives the call.
		std::vector<MeshJob> jobs;
		jobs.reserve(batch.size());
		for (const MeshRequest &r : batch)
			jobs.push_back({r.chunk, r.ops.data(), static_cast<int>(r.ops.size())});

		std::vector<MeshResult> out;
		if (pass.submit(jobs.data(), static_cast<int>(jobs.size()))) {
			pass.collect(&out); // submit/sync/readback: all blocking, all on THIS thread
			last_collect_ms_.store(pass.last_collect_ms(), std::memory_order_relaxed);
		} else {
			// Report a failure per chunk rather than dropping the batch: the caller clears
			// its in-flight markers from the results, so a silent drop would strand them.
			for (const MeshRequest &r : batch) {
				MeshResult f;
				f.chunk = r.chunk;
				f.failed = true;
				out.push_back(std::move(f));
			}
		}
		{
			std::lock_guard<std::mutex> lock(mu_);
			for (MeshResult &r : out) results_.push_back(std::move(r));
			busy_.store(false, std::memory_order_release);
		}
		// run_sync waits on busy_ going false as well as on its own turn.
		done_cv_.notify_all();
	}

	pass.teardown();
	memdelete(rd);
}
