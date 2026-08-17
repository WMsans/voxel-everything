#include "render/mesh_service.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <utility>

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
		extract_busy_.store(false, std::memory_order_release);
		extract_available_.store(false, std::memory_order_release);
		fail_extract_submit_.store(false, std::memory_order_release);
		lod_busy_.store(false, std::memory_order_release);
		lod_available_.store(false, std::memory_order_release);
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
	pending_extract_.clear();
	extract_results_.clear();
	pending_lod_.clear();
	lod_results_.clear();
	pending_volumes_.clear();
#ifdef DEBUG_ENABLED
	submitted_volume_slots_.clear();
#endif
	ready_.store(false, std::memory_order_release);
	busy_.store(false, std::memory_order_release);
	extract_busy_.store(false, std::memory_order_release);
	extract_available_.store(false, std::memory_order_release);
	fail_extracts_.store(false, std::memory_order_release);
	fail_extract_submit_.store(false, std::memory_order_release);
	lod_busy_.store(false, std::memory_order_release);
	lod_available_.store(false, std::memory_order_release);
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

bool MeshService::submit_extracts(std::vector<IslandExtractJob> jobs) {
	if (jobs.empty() || !is_valid()) return false;
	if (fail_extract_submit_.load(std::memory_order_acquire)) return false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_ || !pending_extract_.empty() ||
				extract_busy_.load(std::memory_order_acquire))
			return false;
		pending_extract_ = std::move(jobs);
		extract_busy_.store(true, std::memory_order_release);
	}
	cv_.notify_one();
	return true;
}

int MeshService::collect_extracts(std::vector<IslandExtractResult> *out) {
	std::vector<IslandExtractResult> got;
	{
		std::lock_guard<std::mutex> lock(mu_);
		got.swap(extract_results_);
	}
	const int n = static_cast<int>(got.size());
	if (out)
		for (IslandExtractResult &r : got) out->push_back(std::move(r));
	return n;
}

bool MeshService::submit_lod(std::vector<LodBuildJob> jobs) {
	if (jobs.empty() || !is_valid()) return false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_ || !pending_lod_.empty() || lod_busy_.load(std::memory_order_acquire))
			return false;
		pending_lod_ = std::move(jobs);
		lod_busy_.store(true, std::memory_order_release);
	}
	cv_.notify_one();
	return true;
}

int MeshService::collect_lod(std::vector<LodBuildResult> *out) {
	std::vector<LodBuildResult> got;
	{
		std::lock_guard<std::mutex> lock(mu_);
		got.swap(lod_results_);
	}
	const int n = static_cast<int>(got.size());
	if (out)
		for (LodBuildResult &r : got) out->push_back(std::move(r));
	return n;
}

bool MeshService::submit_volume(int slot, ve::VolumeData data) {
	if (!is_valid() || !data.valid()) return false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_) return false;
		pending_volumes_.push_back({slot, std::move(data)});
#ifdef DEBUG_ENABLED
		submitted_volume_slots_.push_back(slot);
#endif
	}
	cv_.notify_one();
	return true;
}

void MeshService::discard_pending_volume_upload(int slot) {
	std::lock_guard<std::mutex> lock(mu_);
	pending_volumes_.erase(std::remove_if(pending_volumes_.begin(), pending_volumes_.end(),
								  [slot](const VolumeUpload &u) { return u.slot == slot; }),
			pending_volumes_.end());
}

std::vector<int> MeshService::debug_submitted_volume_slots() const {
#ifdef DEBUG_ENABLED
	std::lock_guard<std::mutex> lock(mu_);
	return submitted_volume_slots_;
#else
	return {};
#endif
}

bool MeshService::run_sync(const std::function<void(MeshPass &)> &fn) {
	if (!is_valid()) return false;
	std::unique_lock<std::mutex> lock(mu_);
	if (stopping_) return false;
	// One at a time, and never while a batch, an extraction, a LoD build, or a queued volume
	// upload is running: the diagnostic hooks share the passes' single lattice/cell maps with
	// the streaming path, and volume uploads must land before a diagnostic sees the field.
	done_cv_.wait(lock, [this] {
		return stopping_ || (!sync_pending_ && !busy_.load(std::memory_order_acquire) &&
				!extract_busy_.load(std::memory_order_acquire) &&
				!lod_busy_.load(std::memory_order_acquire) && pending_extract_.empty() &&
				pending_lod_.empty() && pending_volumes_.empty());
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
	if (rd && ok) {
		extract_ = new IslandExtractPass();
		if (extract_->initialize(rd, &pass.volumes())) {
			extract_available_.store(true, std::memory_order_release);
		} else {
			UtilityFunctions::printerr("MeshService: island extraction unavailable");
			delete extract_;
			extract_ = nullptr;
			// Not fatal: collision meshing still works, and IslandManager drops connectivity
			// work it knows can never make progress.
		}
		lod_ = new LodBuildPass();
		LodBuildConfig lod_cfg;
		if (lod_->initialize(rd, lod_cfg)) {
			lod_available_.store(true, std::memory_order_release);
		} else {
			UtilityFunctions::printerr("MeshService: LoD builds unavailable");
			delete lod_;
			lod_ = nullptr;
			// Not fatal: collision meshing still works, and the far-field LoD chain simply
			// cannot submit work.
		}
	}
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
		std::vector<VolumeUpload> volumes_to_upload;
		std::vector<IslandExtractJob> extracts;
		std::vector<MeshRequest> batch;
		std::vector<LodBuildJob> lod_jobs;
		const std::function<void(MeshPass &)> *sync_fn = nullptr;
		{
			std::unique_lock<std::mutex> lock(mu_);
			cv_.wait(lock, [this] {
				return stopping_ || sync_pending_ || !pending_volumes_.empty() ||
						!pending_extract_.empty() || !pending_.empty() || !pending_lod_.empty();
			});
			if (stopping_) break;
			if (sync_pending_) {
				sync_fn = sync_fn_;
			} else if (!pending_volumes_.empty()) {
				volumes_to_upload.swap(pending_volumes_);
			} else if (!pending_extract_.empty()) {
				extracts.swap(pending_extract_);
			} else if (!pending_.empty()) {
				batch.swap(pending_);
			} else if (!pending_lod_.empty()) {
				lod_jobs.swap(pending_lod_);
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

		if (!volumes_to_upload.empty()) {
			for (const VolumeUpload &vu : volumes_to_upload) {
				if (!pass.upload_volume(vu.slot, vu.data))
					UtilityFunctions::printerr("MeshService: volume upload failed for slot ",
							vu.slot);
				if (lod_ && !lod_->volumes().upload(rd, vu.slot, vu.data))
					UtilityFunctions::printerr("MeshService: LoD volume upload failed for slot ",
							vu.slot);
			}
			// run_sync waits on pending_volumes_ going empty as well as on its own turn.
			done_cv_.notify_all();
			continue;
		}

		if (!extracts.empty()) {
			std::vector<IslandExtractResult> extract_out;
			extract_out.reserve(extracts.size());
			for (const IslandExtractJob &job : extracts) {
				if (job.kind == kResampleVolume) {
					IslandExtractResult r;
					r.id = job.id;
					r.kind = job.kind;
					r.failed = !ve::resample_volume(job.source, job.source_op, job.basis,
							job.rest_origin, job.out_slot, job.dim, &r.data, &r.op);
					extract_out.push_back(std::move(r));
				} else if (extract_) {
					IslandExtractResult r;
					if (fail_extracts_.load(std::memory_order_acquire)) {
						r.id = job.id;
						r.kind = job.kind;
						r.failed = true;
					} else {
						extract_->extract(job, &r);
						r.kind = job.kind;
					}
					extract_out.push_back(std::move(r));
				} else {
					IslandExtractResult r;
					r.id = job.id;
					r.kind = job.kind;
					r.failed = true;
					extract_out.push_back(std::move(r));
				}
			}
			{
				std::lock_guard<std::mutex> lock(mu_);
				for (IslandExtractResult &r : extract_out) extract_results_.push_back(std::move(r));
				extract_busy_.store(false, std::memory_order_release);
			}
			// run_sync waits on extract_busy_ going false as well as on its own turn.
			done_cv_.notify_all();
			continue;
		}

		// Point the jobs at the ops this batch owns; `batch` outlives the call.
		std::vector<MeshJob> jobs;
		jobs.reserve(batch.size());
		for (const MeshRequest &r : batch) {
			MeshJob job;
			job.chunk = r.chunk;
			job.ops = r.ops.data();
			job.op_count = static_cast<int>(r.ops.size());
			ve::chunk_world_origin(r.chunk, job.origin);
			job.cell_size = ve::kChunkCellSize;
			job.lattice = ve::kChunkLattice;
			jobs.push_back(job);
		}

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

		if (!lod_jobs.empty()) {
			std::vector<LodBuildResult> lod_out;
			if (lod_ && lod_->submit(lod_jobs.data(), static_cast<int>(lod_jobs.size()))) {
				lod_->collect(&lod_out);
			} else {
				// Report a failure per job rather than dropping the batch: the caller clears
				// its in-flight markers from the results, so a silent drop would strand them.
				for (const LodBuildJob &job : lod_jobs) {
					LodBuildResult f;
					f.level = job.level;
					f.coord = job.coord;
					f.failed = true;
					lod_out.push_back(std::move(f));
				}
			}
			{
				std::lock_guard<std::mutex> lock(mu_);
				for (LodBuildResult &r : lod_out) lod_results_.push_back(std::move(r));
				lod_busy_.store(false, std::memory_order_release);
			}
			// run_sync waits on lod_busy_ going false as well as on its own turn.
			done_cv_.notify_all();
		}
	}

	if (extract_) {
		delete extract_;
		extract_ = nullptr;
	}
	if (lod_) {
		delete lod_;
		lod_ = nullptr;
	}
	pass.teardown();
	memdelete(rd);
}
