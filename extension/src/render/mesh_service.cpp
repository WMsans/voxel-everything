#include "render/mesh_service.h"
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <utility>

using namespace godot;

// LoD chunks per GPU batch. Task 12 plans to submit up to lod_builds_per_frame (default 8)
// jobs per frame, so the pass must be configured to accept that many in a single batch.
constexpr int kLodMaxJobsPerBatch = 8;

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
		consolidate_busy_.store(false, std::memory_order_release);
		fail_consolidations_.store(false, std::memory_order_release);
		fail_consolidate_uploads_.store(false, std::memory_order_release);
		fail_next_restore_.store(false, std::memory_order_release);
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
	pending_override_updates_.clear();
	pending_consolidations_.clear();
	consolidate_results_.clear();
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
	consolidate_busy_.store(false, std::memory_order_release);
	fail_consolidations_.store(false, std::memory_order_release);
	fail_consolidate_uploads_.store(false, std::memory_order_release);
	fail_next_restore_.store(false, std::memory_order_release);
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

bool MeshService::submit_consolidations(std::vector<ConsolidateJob> jobs) {
	if (jobs.empty() || !is_valid()) return false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_ || !pending_consolidations_.empty() ||
				consolidate_busy_.load(std::memory_order_acquire)) return false;
		pending_consolidations_ = std::move(jobs);
		consolidate_busy_.store(true, std::memory_order_release);
	}
	cv_.notify_one();
	return true;
}

bool MeshService::set_override_region(ve::IVec3 region, int region_slot, int table,
		const std::vector<std::pair<int, int>> &entries) {
	if (region_slot < 0 || table < -1 || table >= OverridePool::kMaxOverrideTables ||
			!is_valid()) return false;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_) return false;
		OverrideUpdate update;
		update.region = region;
		update.region_slot = region_slot;
		update.table = table;
		update.entries = entries;
		pending_override_updates_.push_back(std::move(update));
	}
	cv_.notify_one();
	return true;
}

void MeshService::clear_override_region(int region_slot) {
	if (region_slot < 0 || !is_valid()) return;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (stopping_) return;
		OverrideUpdate update;
		update.clear = true;
		update.region_slot = region_slot;
		pending_override_updates_.push_back(std::move(update));
	}
	cv_.notify_one();
}

bool MeshService::replay_overrides(const ve::OverrideStore &store,
		const std::map<std::tuple<int, int, int>, int> &tables) {
	bool uploaded = true;
	const bool ran = run_sync([&](MeshPass &pass) {
		for (int table = 0; table < OverridePool::kMaxOverrideTables; table++)
			pass.clear_override_table(table);
		for (const auto &region_table : tables) {
			const ve::IVec3 region{std::get<0>(region_table.first), std::get<1>(region_table.first),
					std::get<2>(region_table.first)};
			const int table = region_table.second;
			if (table < 0 || table >= OverridePool::kMaxOverrideTables) {
				uploaded = false;
				return;
			}
			const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
					region.z * ve::kRegionBricks};
			for (int z = 0; z < ve::kRegionBricks; z++)
				for (int y = 0; y < ve::kRegionBricks; y++)
					for (int x = 0; x < ve::kRegionBricks; x++) {
						const ve::IVec3 brick{base.x + x, base.y + y, base.z + z};
						const int slot = store.slot_of(brick);
						if (slot < 0) continue;
						const ve::OverrideBrick *data = store.data(slot);
						if (!data || !pass.upload_override(slot, *data)) {
							uploaded = false;
							return;
						}
						pass.set_override_entry(table, ve::WorldBounds::brick_index_in_region(brick), slot);
					}
			// The region-slot map is intentionally not guessed here; residency restores it on
			// stream-in, while the table bytes are safe to replay globally.
		}
	});
	if (ran && uploaded) override_tables_ = tables;
	return ran && uploaded;
}

bool MeshService::restore_overrides(const std::vector<int> &slots,
		const std::vector<ve::OverrideBrick> &bricks, ve::IVec3 region, int region_slot,
		int table, int old_table, const std::vector<std::pair<int, int>> &old_entries) {
	if (slots.size() != bricks.size() || region_slot < 0 || table < 0 ||
			table >= OverridePool::kMaxOverrideTables)
		return false;
	// Failure injection is deliberately before run_sync: a failed restore must not mutate
	// either worker consumer, so the caller can retry the whole rollback transaction.
	if (fail_next_restore_.exchange(false, std::memory_order_acq_rel)) return false;
	bool uploaded = true;
	const bool ran = run_sync([&](MeshPass &pass) {
		for (size_t i = 0; i < slots.size(); i++) {
			if (!pass.upload_override(slots[i], bricks[i]) ||
					(lod_ && !lod_->upload_override(slots[i], bricks[i]))) {
				uploaded = false;
				return;
			}
		}
		if (!uploaded) return;
		pass.clear_override_table(table);
		pass.set_override_table(region_slot, old_table, old_entries);
		if (lod_) lod_->set_override_table(region_slot, old_table, old_entries);
		if (old_table < 0)
			override_tables_.erase(std::tuple<int, int, int>{region.x, region.y, region.z});
		else
			override_tables_[std::tuple<int, int, int>{region.x, region.y, region.z}] = old_table;
	});
	if (!ran || !uploaded)
		UtilityFunctions::printerr("MeshService: restore override transaction failed (ran=", ran,
				", uploaded=", uploaded, ", slots=", static_cast<int>(slots.size()), ", table=", table,
				", old_table=", old_table, ")");
	return ran && uploaded;
}

bool MeshService::publish_overrides(const std::vector<int> &slots,
		const std::vector<ve::OverrideBrick> &bricks, ve::IVec3 region, int region_slot,
		int table, const std::vector<std::pair<int, int>> &entries) {
	if (slots.size() != bricks.size() || slots.empty() || region_slot < 0 || table < 0) return false;
	bool uploaded = !fail_consolidate_uploads_.load(std::memory_order_acquire);
	const bool ran = run_sync([&](MeshPass &pass) {
		if (!uploaded) return;
		for (size_t i = 0; i < slots.size(); i++) {
			if (!pass.upload_override(slots[i], bricks[i]) ||
					(lod_ && !lod_->upload_override(slots[i], bricks[i]))) {
				uploaded = false;
				return;
			}
		}
		// Do not expose a table until the complete replacement is present on every worker
		// consumer. A later retry may overwrite the uploaded bytes, but cannot observe a
		// half-published region.
		pass.set_override_table(region_slot, table, entries);
		if (lod_) lod_->set_override_table(region_slot, table, entries);
		override_tables_[std::tuple<int, int, int>{region.x, region.y, region.z}] = table;
	});
	return ran && uploaded;
}

bool MeshService::publish_override(int slot, const ve::OverrideBrick &brick, ve::IVec3 region,
		int region_slot, int table, const std::vector<std::pair<int, int>> &entries) {
	return publish_overrides(std::vector<int>{slot}, std::vector<ve::OverrideBrick>{brick}, region,
			region_slot, table, entries);
}

int MeshService::collect_consolidations(std::vector<ConsolidateResult> *out) {
	std::vector<ConsolidateResult> got;
	{
		std::lock_guard<std::mutex> lock(mu_);
		got.swap(consolidate_results_);
	}
	const int n = static_cast<int>(got.size());
	if (out)
		for (ConsolidateResult &r : got) out->push_back(std::move(r));
	return n;
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
				!lod_busy_.load(std::memory_order_acquire) &&
				!consolidate_busy_.load(std::memory_order_acquire) && pending_extract_.empty() &&
				pending_lod_.empty() && pending_override_updates_.empty() &&
				pending_consolidations_.empty() && pending_volumes_.empty());
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
		extract_->set_override_pool(&pass.overrides());
		if (extract_->initialize(rd, &pass.volumes())) {
			extract_available_.store(true, std::memory_order_release);
		} else {
			UtilityFunctions::printerr("MeshService: island extraction unavailable");
			delete extract_;
			extract_ = nullptr;
		}
		lod_ = new LodBuildPass();
		lod_->set_override_pool(&pass.overrides());
		LodBuildConfig lod_cfg;
		lod_cfg.max_jobs = kLodMaxJobsPerBatch;
		if (lod_->initialize(rd, lod_cfg)) {
			lod_available_.store(true, std::memory_order_release);
		} else {
			UtilityFunctions::printerr("MeshService: LoD builds unavailable");
			delete lod_;
			lod_ = nullptr;
		}
		consolidate_ = new ConsolidatePass();
		if (!consolidate_->initialize(rd, &pass.overrides())) {
			UtilityFunctions::printerr("MeshService: consolidation unavailable");
			delete consolidate_;
			consolidate_ = nullptr;
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
		std::vector<OverrideUpdate> override_updates;
		std::vector<ConsolidateJob> consolidate_jobs;
		const std::function<void(MeshPass &)> *sync_fn = nullptr;
		{
			std::unique_lock<std::mutex> lock(mu_);
			cv_.wait(lock, [this] {
				return stopping_ || sync_pending_ || !pending_volumes_.empty() ||
						!pending_extract_.empty() || !pending_.empty() || !pending_lod_.empty() ||
						!pending_override_updates_.empty() || !pending_consolidations_.empty();
			});
			if (stopping_) break;
			if (sync_pending_) {
				sync_fn = sync_fn_;
			} else if (!pending_override_updates_.empty()) {
				override_updates.swap(pending_override_updates_);
			} else if (!pending_volumes_.empty()) {
				volumes_to_upload.swap(pending_volumes_);
			} else if (!pending_extract_.empty()) {
				extracts.swap(pending_extract_);
			} else if (!pending_.empty()) {
				batch.swap(pending_);
			} else if (!pending_lod_.empty()) {
				lod_jobs.swap(pending_lod_);
			} else if (!pending_consolidations_.empty()) {
				consolidate_jobs.swap(pending_consolidations_);
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

		if (!override_updates.empty()) {
			for (const OverrideUpdate &update : override_updates) {
				if (update.clear) {
					pass.clear_override_region(update.region_slot);
					continue;
				}
				pass.set_override_table(update.region_slot, update.table, update.entries);
				if (update.table < 0)
					override_tables_.erase(std::tuple<int, int, int>{update.region.x,
							update.region.y, update.region.z});
				else
					override_tables_[std::tuple<int, int, int>{update.region.x,
							update.region.y, update.region.z}] = update.table;
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

		if (!consolidate_jobs.empty()) {
			std::vector<ConsolidateResult> consolidate_out;
			for (const ConsolidateJob &job : consolidate_jobs) {
				ConsolidateResult r;
				if (fail_consolidations_.load(std::memory_order_acquire) || !consolidate_ ||
						!consolidate_->run(job, &r)) r.failed = true;
				consolidate_out.push_back(std::move(r));
			}
			{
				std::lock_guard<std::mutex> lock(mu_);
				for (ConsolidateResult &r : consolidate_out) consolidate_results_.push_back(std::move(r));
				consolidate_busy_.store(false, std::memory_order_release);
			}
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
			const ve::IVec3 rr = ve::region_of_chunk(r.chunk);
			job.cell_size = ve::kChunkCellSize;
			job.lattice = ve::kChunkLattice;
			auto table_it = override_tables_.find(std::tuple<int, int, int>{rr.x, rr.y, rr.z});
			job.override_table = table_it == override_tables_.end() ? -1 : table_it->second;
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
			for (LodBuildJob &job : lod_jobs) {
				float origin[3];
				ve::lod_chunk_origin(job.level, job.coord, origin);
				const ve::IVec3 rr = ve::WorldBounds::region_of_point(origin[0], origin[1], origin[2]);
				auto table_it = override_tables_.find(std::tuple<int, int, int>{rr.x, rr.y, rr.z});
				if (job.override_table < 0 && table_it != override_tables_.end())
					job.override_table = table_it->second;
			}
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
	if (consolidate_) {
		delete consolidate_;
		consolidate_ = nullptr;
	}
	pass.teardown();
	memdelete(rd);
}
