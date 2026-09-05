#include "mesh/consolidation.h"

#include <algorithm>
#include <chrono>
#include <thread>

#include <godot_cpp/variant/utility_functions.hpp>

#include "generator/edit_ops.h"
#include "lod/lod_tree.h"
#include "render/gpu_atlas.h"
#include "render/mesh_service.h"
#include "render/world_streamer.h"
#include "world/edit_log.h"

namespace godot {

ConsolidationCoordinator::ConsolidationCoordinator(WorldStore *store, Collaborators handles)
		: store_(store), handles_(handles) {}

bool ConsolidationCoordinator::queue_consolidation(ve::IVec3 region) {
	if (consolidation_in_flight_ && consolidation_job_.region == region) return false;
	for (const ve::IVec3 &queued : consolidation_queue_)
		if (queued == region) return false;
	if (static_cast<int>(consolidation_queue_.size()) + (consolidation_in_flight_ ? 1 : 0) >=
			OverridePool::kMaxOverrideTables) {
		consolidation_queue_refusals_++;
		if (!consolidation_queue_refusal_logged_) {
			UtilityFunctions::printerr("VoxelWorld: consolidation queue full; refusing new region once");
			consolidation_queue_refusal_logged_ = true;
		}
		return false;
	}
	consolidation_queue_.push_back(region);
	return true;
}

void ConsolidationCoordinator::requeue_consolidation_locked(ve::IVec3 region) {
	for (const ve::IVec3 &queued : consolidation_queue_)
		if (queued == region) return;
	if (static_cast<int>(consolidation_queue_.size()) < OverridePool::kMaxOverrideTables)
		consolidation_queue_.insert(consolidation_queue_.begin(), region);
	else {
		consolidation_queue_refusals_++;
		if (!consolidation_queue_refusal_logged_) {
			UtilityFunctions::printerr("VoxelWorld: consolidation rollback could not requeue region once");
			consolidation_queue_refusal_logged_ = true;
		}
	}
}

void ConsolidationCoordinator::pump_async() {
	// This is a frame-pump path: the lifetime guard prevents teardown from racing the
	// transaction, while the edit lock makes the queue, log, CPU store, and table map one
	// consistent state. No worker call below waits for GPU work.
	std::unique_lock<std::mutex> lifetime(*handles_.render_lifetime_mutex);
	if (*handles_.render_shutting_down) return;
	std::unique_lock<std::mutex> edit_lock(store_->edit_mutex());
	if (!mesh() || !mesh()->is_valid() || !store_->edit_log() || !store_->overrides() ||
			!store_->residency())
		return;

	const auto reset_transaction = [this]() {
		consolidation_in_flight_ = false;
		consolidation_publish_in_flight_ = false;
		consolidation_job_ = ConsolidateJob{};
		consolidation_table_ = -1;
		consolidation_old_table_ = -1;
		consolidation_old_entries_.clear();
		consolidation_entries_.clear();
		consolidation_old_slots_.clear();
		consolidation_old_bricks_.clear();
		consolidation_newly_acquired_.clear();
		consolidation_slots_.clear();
		consolidation_baked_.clear();
	};
	const auto requeue = [this](ve::IVec3 region) { requeue_consolidation_locked(region); };
	const auto rollback_render = [this]() {
		bool ok = true;
		if (atlas()) {
			for (size_t i = 0; i < consolidation_old_slots_.size(); i++)
				if (!atlas()->upload_override(device(), consolidation_old_slots_[i],
						consolidation_old_bricks_[i])) ok = false;
			// Restore the previous NORMAL handles alongside the previous override bytes:
			// re-upload each old brick's compact normals, or park -1 where the old brick
			// had none, so a rolled-back table never names stale spans.
			for (size_t i = 0; i < consolidation_old_slots_.size(); i++) {
				const ve::OverrideBrick &old = consolidation_old_bricks_[i];
				if (old.normal_oct.size() == ve::kBrickSdfCount)
					atlas()->stored_normals().upload_override(device(), consolidation_old_slots_[i],
							old.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas()->stored_normals().release_override(device(), consolidation_old_slots_[i]);
			}
			if (consolidation_table_ >= 0) atlas()->overrides().clear_table(device(), consolidation_table_);
			if (consolidation_job_.region_slot >= 0)
				atlas()->set_override_table(device(), consolidation_job_.region_slot,
						consolidation_old_table_, consolidation_old_entries_);
		}
		return ok;
	};
	const auto refuse_transaction = [&](bool retry, bool rebuild_worker) {
		const ve::IVec3 region = consolidation_job_.region;
		const bool restored = rollback_render();
		for (const ve::IVec3 brick : consolidation_newly_acquired_) {
			// The speculative slot's normal span was staged before the table entry naming
			// it. Releasing the slot without releasing the span leaks payload out of the
			// fixed 32 MiB pool for the rest of the process.
			const int slot = store_->overrides() ? store_->overrides()->slot_of(brick) : -1;
			if (slot >= 0 && atlas()) atlas()->stored_normals().release_override(device(), slot);
			store_->overrides()->release(brick);
		}
		if (!restored)
			UtilityFunctions::printerr(
					"VoxelWorld: render override rollback failed; retaining old edit state");
		// A worker rollback failure means its bytes are not authoritative. Rebuild from the
		// CPU store/table map after releasing the speculative slots and before any requeue.
		bool worker_rebuilt = true;
		if (rebuild_worker)
			worker_rebuilt = mesh()->replay_overrides(*store_->overrides(), store_->override_tables());
		if (!worker_rebuilt)
			UtilityFunctions::printerr(
					"VoxelWorld: worker override rebuild failed; refusing requeue");
		consolidation_refusals_++;
		if (retry && worker_rebuilt) requeue(region);
		reset_transaction();
	};

	if (consolidation_in_flight_) {
		const ve::IVec3 region = consolidation_job_.region;
		if (consolidation_publish_in_flight_) {
			std::vector<OverridePublicationResult> results;
			if (mesh()->collect_override_publications(&results) == 0) return;
			if (results.empty() || !results.front().success) {
				const bool rebuild_worker = !results.empty() && !results.front().worker_state_valid;
				refuse_transaction(true, rebuild_worker);
				return;
			}
			// The worker transaction is complete. CPU bytes are committed only now; the old
			// table and op list were untouched until both consumers succeeded.
			const ve::IVec3 r = region;
			const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
					r.z * ve::kRegionBricks};
			// The baked bytes live in the transaction's slots through the worker command; copy
			// them from the publication command's result is unnecessary because acquire slots
			// were populated before submission below.
			if (store_->residency()->slot_of(r) != consolidation_job_.region_slot) {
				refuse_transaction(true, false);
				return;
			}
			if (atlas())
				atlas()->set_override_table(device(), consolidation_job_.region_slot,
						consolidation_table_, consolidation_entries_);
			for (size_t i = 0; i < consolidation_slots_.size(); i++)
				if (ve::OverrideBrick *data = store_->overrides()->data(consolidation_slots_[i]))
					*data = consolidation_baked_[i];
			store_->edit_log()->clear_region_through(r, consolidation_job_.through_seq);
			pending_dirty().push_back({ve::chunk_of_brick(base),
					ve::chunk_of_brick({base.x + ve::kRegionBricks - 1,
							base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1})});
			if (store_->edit_log()->op_count(r) >= ve::kConsolidateAtOps) queue_consolidation(r);
			float lo[3], first_hi[3], last_lo[3], hi[3];
			ve::brick_world_aabb(base, lo, first_hi);
			ve::brick_world_aabb({base.x + ve::kRegionBricks - 1,
					base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1}, last_lo, hi);
			if (lod_tree()) {
				std::lock_guard<std::mutex> lod_lock(lod_mutex());
				lod_tree()->mark_dirty(lo, hi);
			}
			if (streamer()) streamer()->queue_region_regeneration_locked(r);
			store_->override_tables()[std::tuple<int, int, int>{r.x, r.y, r.z}] = consolidation_table_;
			consolidation_count_++;
			reset_transaction();
			return;
		}

		std::vector<ConsolidateResult> results;
		if (mesh()->collect_consolidations(&results) == 0) return;
		if (results.empty() || results.front().failed ||
				results.front().baked.size() != consolidation_job_.bricks.size()) {
			consolidation_refusals_++;
			requeue(region);
			reset_transaction();
			return;
		}
		const ConsolidateResult &result = results.front();
		consolidation_slots_.clear();
		consolidation_baked_ = results.front().baked;
		consolidation_newly_acquired_.clear();
		for (const ve::IVec3 brick : result.bricks) {
			const bool present = store_->overrides()->slot_of(brick) >= 0;
			const int slot = store_->overrides()->acquire(brick);
			if (slot < 0) {
				refuse_transaction(true, false);
				return;
			}
			if (!present) consolidation_newly_acquired_.push_back(brick);
			consolidation_slots_.push_back(slot);
		}
		for (size_t i = 0; i < result.bricks.size(); i++) {
			const int bi = ve::brick_index_in_region(result.bricks[i]);
			consolidation_entries_.erase(std::remove_if(consolidation_entries_.begin(),
					consolidation_entries_.end(), [bi](const std::pair<int, int> &entry) {
						return entry.first == bi;
					}), consolidation_entries_.end());
			consolidation_entries_.emplace_back(bi, consolidation_slots_[i]);
		}
		if (store_->residency()->slot_of(region) != consolidation_job_.region_slot) {
			refuse_transaction(true, false);
			return;
		}
		bool render_ok = true;
		if (atlas())
			for (size_t i = 0; i < consolidation_slots_.size(); i++)
				if (!atlas()->upload_override(device(), consolidation_slots_[i], result.baked[i])) render_ok = false;
		// Task 7: stage the baked compact normals in the SAME transaction, before the
		// table entry that names them is published. A failed normal upload parks -1 and
		// the shader falls back to R8 taps -- geometry is never rejected.
		if (atlas())
			for (size_t i = 0; i < consolidation_slots_.size(); i++) {
				const ve::OverrideBrick &brick = consolidation_baked_[i];
				if (brick.normal_oct.size() == ve::kBrickSdfCount)
					atlas()->stored_normals().upload_override(device(), consolidation_slots_[i],
							brick.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas()->stored_normals().release_override(device(), consolidation_slots_[i]);
			}
		if (!render_ok) {
			refuse_transaction(true, false);
			return;
		}
		OverridePublication publication;
		publication.slots = consolidation_slots_;
		publication.bricks = consolidation_baked_;
		publication.old_slots = consolidation_old_slots_;
		publication.old_bricks = consolidation_old_bricks_;
		publication.region = region;
		publication.region_slot = consolidation_job_.region_slot;
		publication.table = consolidation_table_;
		publication.old_table = consolidation_old_table_;
		publication.entries = consolidation_entries_;
		publication.old_entries = consolidation_old_entries_;
		if (!mesh()->submit_override_publication(std::move(publication))) {
			refuse_transaction(true, false);
			return;
		}
		consolidation_publish_in_flight_ = true;
		return;
	}

	if (consolidation_queue_.empty()) return;
	const ve::IVec3 region = consolidation_queue_.front();
	consolidation_queue_.erase(consolidation_queue_.begin());
	const int region_slot = store_->residency()->slot_of(region);
	if (region_slot < 0) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	const std::vector<ve::EditOp> &ops = store_->edit_log()->ops(region);
	if (ops.empty()) return;
	ConsolidateJob job;
	job.region = region;
	job.region_slot = region_slot;
	job.ops = ops;
	const std::vector<uint64_t> &seqs = store_->edit_log()->seqs(region);
	job.through_seq = seqs.empty() ? 0 : seqs.back();
	job.gen = &store_->generator()->sampler();
	ve::plan_consolidation(job.ops.data(), static_cast<int>(job.ops.size()), region, &job.bricks);
	if (!job.bricks.empty()) {
		// Spec requires collect + snapshot while edit_mutex_ is held: edit_lock above spans this
		// whole function, so the overrides, edit log, and volumes are read in one consistent state.
		ve::IVec3 lo = job.bricks[0], hi = job.bricks[0];
		for (const auto &b : job.bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!store_->snapshot_field_sources(job.ops, lo, hi, &job.source)) {
			consolidation_refusals_++;
			requeue(region);
			return;
		}
	}
	int needed_slots = 0;
	for (const ve::IVec3 brick : job.bricks) if (store_->overrides()->slot_of(brick) < 0) needed_slots++;
	if (job.bricks.empty() || needed_slots > store_->overrides()->capacity() - store_->overrides()->used()) {
		consolidation_refusals_++;
		requeue(region);
		return;
	}
	const std::tuple<int, int, int> key{region.x, region.y, region.z};
	const auto found = store_->override_tables().find(key);
	const int old_table = found == store_->override_tables().end() ? -1 : found->second;
	int table = old_table;
	if (table < 0) {
		std::vector<bool> used(OverridePool::kMaxOverrideTables, false);
		for (const auto &it : store_->override_tables())
			if (it.second >= 0 && it.second < OverridePool::kMaxOverrideTables)
				used[static_cast<size_t>(it.second)] = true;
		for (int i = 0; i < OverridePool::kMaxOverrideTables; i++)
			if (!used[static_cast<size_t>(i)]) { table = i; break; }
		if (table < 0) {
			consolidation_refusals_++;
			requeue(region);
			return;
		}
	}
	const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
			region.z * ve::kRegionBricks};
	consolidation_old_entries_.clear();
	consolidation_old_slots_.clear();
	consolidation_old_bricks_.clear();
	for (int z = 0; z < ve::kRegionBricks; z++)
		for (int y = 0; y < ve::kRegionBricks; y++)
			for (int x = 0; x < ve::kRegionBricks; x++) {
				const ve::IVec3 brick{base.x + x, base.y + y, base.z + z};
				const int slot = store_->overrides()->slot_of(brick);
				if (slot < 0) continue;
				consolidation_old_entries_.emplace_back(
						ve::brick_index_in_region(brick), slot);
				consolidation_old_slots_.push_back(slot);
				consolidation_old_bricks_.push_back(*store_->overrides()->data(slot));
			}
	consolidation_job_ = std::move(job);
	consolidation_table_ = table;
	consolidation_old_table_ = old_table;
	consolidation_entries_ = consolidation_old_entries_;
	std::vector<ConsolidateJob> worker_jobs;
	worker_jobs.push_back(consolidation_job_);
	if (!mesh()->submit_consolidations(std::move(worker_jobs))) {
		consolidation_refusals_++;
		requeue(region);
		reset_transaction();
		return;
	}
	consolidation_in_flight_ = true;
}

void ConsolidationCoordinator::wait() {
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	for (;;) {
		bool in_flight = false;
		{
			std::lock_guard<std::mutex> lock(store_->edit_mutex());
			in_flight = consolidation_in_flight_;
		}
		if (!in_flight || std::chrono::steady_clock::now() >= deadline) return;
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
		pump_async();
	}
}

void ConsolidationCoordinator::pump() {
	pump_async();
	wait();
}

void ConsolidationCoordinator::rollback_in_flight_for_worker_teardown() {
	// Caller holds edit_mutex (VoxelWorld::teardown_physics takes it across the whole
	// teardown sequence).
	if (consolidation_in_flight_) {
		const ve::IVec3 region = consolidation_job_.region;
		bool render_restored = true;
		if (atlas()) {
			for (size_t i = 0; i < consolidation_old_slots_.size(); i++) {
				if (!atlas()->upload_override(device(), consolidation_old_slots_[i],
						consolidation_old_bricks_[i])) render_restored = false;
				// Restore the NORMAL handle alongside the bytes, exactly as
				// rollback_render() does. Leaving the new bake's normals bound to a slot
				// holding the old brick's SDF/material shades a surface that is not there.
				const ve::OverrideBrick &old_brick = consolidation_old_bricks_[i];
				if (old_brick.normal_oct.size() == ve::kBrickSdfCount)
					atlas()->stored_normals().upload_override(device(),
							consolidation_old_slots_[i], old_brick.normal_oct.data(),
							ve::kBrickSdfCount);
				else
					atlas()->stored_normals().release_override(device(),
							consolidation_old_slots_[i]);
			}
			if (consolidation_table_ >= 0)
				atlas()->overrides().clear_table(device(), consolidation_table_);
			if (consolidation_job_.region_slot >= 0) {
				if (render_restored)
					atlas()->set_override_table(device(), consolidation_job_.region_slot,
							consolidation_old_table_, consolidation_old_entries_);
				else
					// A failed byte restore must not expose the partial new table. The CPU
					// store/map remain authoritative and reinit will replay them.
					atlas()->set_override_table(device(), consolidation_job_.region_slot, -1, {});
			}
		}
		if (!render_restored)
			UtilityFunctions::printerr(
					"VoxelWorld: render override rollback failed during physics teardown; invalidated table");
		for (const ve::IVec3 brick : consolidation_newly_acquired_) {
			const int slot = store_->overrides() ? store_->overrides()->slot_of(brick) : -1;
			if (slot >= 0 && atlas()) atlas()->stored_normals().release_override(device(), slot);
			store_->overrides()->release(brick);
		}
		if (store_->edit_log() && store_->edit_log()->op_count(region) > 0)
			consolidation_queue_.insert(consolidation_queue_.begin(), region);
		consolidation_in_flight_ = false;
		consolidation_job_ = ConsolidateJob{};
		consolidation_table_ = -1;
		consolidation_old_table_ = -1;
		consolidation_old_entries_.clear();
		consolidation_entries_.clear();
		consolidation_old_slots_.clear();
		consolidation_old_bricks_.clear();
		consolidation_newly_acquired_.clear();
		consolidation_slots_.clear();
		consolidation_baked_.clear();
		consolidation_publish_in_flight_ = false;
	}
}

bool ConsolidationCoordinator::force_region(ve::IVec3 r) {
	std::unique_lock<std::mutex> edit_lock(store_->edit_mutex());
	const auto refuse = [this]() { consolidation_refusals_++; return false; };
	if (!mesh() || !store_->edit_log() || !store_->overrides()) return refuse();
	std::vector<ve::EditOp> ops = store_->edit_log()->ops(r);
	std::vector<ve::IVec3> bricks;
	ve::plan_consolidation(ops.data(), static_cast<int>(ops.size()), r, &bricks);
	int needed_slots = 0;
	for (const ve::IVec3 b : bricks) if (store_->overrides()->slot_of(b) < 0) needed_slots++;
	if (bricks.empty() || needed_slots > store_->overrides()->capacity() - store_->overrides()->used()) return refuse();
	const int resident_slot = store_->residency() ? store_->residency()->slot_of(r) : -1;
	if (resident_slot < 0) return refuse();

	const std::tuple<int, int, int> key{r.x, r.y, r.z};
	const auto found = store_->override_tables().find(key);
	const int old_table = found == store_->override_tables().end() ? -1 : found->second;
	int table = old_table;
	if (table < 0) {
		std::vector<bool> used(OverridePool::kMaxOverrideTables, false);
		for (const auto &it : store_->override_tables())
			if (it.second >= 0 && it.second < OverridePool::kMaxOverrideTables)
				used[static_cast<size_t>(it.second)] = true;
		for (int i = 0; i < OverridePool::kMaxOverrideTables; i++)
			if (!used[static_cast<size_t>(i)]) { table = i; break; }
		if (table < 0) return refuse();
	}
	const ve::IVec3 base{r.x * ve::kRegionBricks, r.y * ve::kRegionBricks,
			r.z * ve::kRegionBricks};
	std::vector<std::pair<int, int>> old_entries;
	std::vector<int> old_slots;
	std::vector<ve::OverrideBrick> old_bricks;
	for (int z = 0; z < ve::kRegionBricks; z++)
		for (int y = 0; y < ve::kRegionBricks; y++)
			for (int x = 0; x < ve::kRegionBricks; x++) {
				const ve::IVec3 b{base.x + x, base.y + y, base.z + z};
				const int slot = store_->overrides()->slot_of(b);
				if (slot < 0) continue;
				old_entries.emplace_back(ve::brick_index_in_region(b), slot);
				old_slots.push_back(slot);
				old_bricks.push_back(*store_->overrides()->data(slot));
			}

	ConsolidateJob job;
	job.region = r;
	job.region_slot = resident_slot;
	job.bricks = bricks;
	job.ops = ops;
	job.gen = &store_->generator()->sampler();
	if (!bricks.empty()) {
		ve::IVec3 lo = bricks[0], hi = bricks[0];
		for (auto &b : bricks) { lo.x = std::min(lo.x, b.x); lo.y = std::min(lo.y, b.y); lo.z = std::min(lo.z, b.z); hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y); hi.z = std::max(hi.z, b.z); }
		if (!store_->snapshot_field_sources(ops, lo, hi, &job.source)) return refuse();
	}
	// A reused worker region slot must see the old table while the bake reads its base.
	if (!mesh()->set_override_region(r, job.region_slot, old_table, old_entries)) return refuse();
	if (!mesh()->submit_consolidations({job})) return refuse();
	mesh()->run_sync([](MeshPass &) {});
	std::vector<ConsolidateResult> results;
	if (mesh()->collect_consolidations(&results) != 1 || results[0].failed ||
			results[0].baked.size() != bricks.size()) return refuse();

	std::vector<int> slots;
	std::vector<ve::IVec3> newly_acquired;
	for (const ve::IVec3 b : bricks) {
		const bool was_present = store_->overrides()->slot_of(b) >= 0;
		const int slot = store_->overrides()->acquire(b);
		if (slot < 0) {
			for (const ve::IVec3 acquired : newly_acquired) store_->overrides()->release(acquired);
			return refuse();
		}
		if (!was_present) newly_acquired.push_back(b);
		slots.push_back(slot);
	}
	std::vector<std::pair<int, int>> entries;
	entries.reserve(bricks.size());
	for (size_t i = 0; i < bricks.size(); i++)
		entries.emplace_back(ve::brick_index_in_region(bricks[i]), slots[i]);

	// Stage render bytes first and check every upload. The old CPU bytes/table remain the
	// rollback source until both devices have published the complete replacement.
	bool render_ok = true;
	if (atlas()) {
		for (size_t i = 0; i < slots.size(); i++)
			if (!atlas()->upload_override(device(), slots[i], results[0].baked[i])) render_ok = false;
		// Task 7: normals share the transaction -- payload before table publication.
		if (render_ok)
			for (size_t i = 0; i < slots.size(); i++) {
				const ve::OverrideBrick &brick = results[0].baked[i];
				if (brick.normal_oct.size() == ve::kBrickSdfCount)
					atlas()->stored_normals().upload_override(device(), slots[i],
							brick.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas()->stored_normals().release_override(device(), slots[i]);
			}
		if (render_ok) atlas()->set_override_table(device(), job.region_slot, table, entries);
	}
	const auto rollback_publication = [&]() {
		bool render_restored = true;
		if (atlas()) {
			for (size_t i = 0; i < old_slots.size(); i++)
				if (!atlas()->upload_override(device(), old_slots[i], old_bricks[i])) render_restored = false;
			// Restore the previous NORMAL handles alongside the previous override bytes.
			for (size_t i = 0; i < old_slots.size(); i++) {
				const ve::OverrideBrick &old = old_bricks[i];
				if (old.normal_oct.size() == ve::kBrickSdfCount)
					atlas()->stored_normals().upload_override(device(), old_slots[i],
							old.normal_oct.data(), ve::kBrickSdfCount);
				else
					atlas()->stored_normals().release_override(device(), old_slots[i]);
			}
			atlas()->overrides().clear_table(device(), table);
			atlas()->set_override_table(device(), job.region_slot, old_table, old_entries);
		}
		bool worker_restored = mesh()->restore_overrides(old_slots, old_bricks, r,
				job.region_slot, table, old_table, old_entries);
		if (!worker_restored) {
			UtilityFunctions::printerr("VoxelWorld: worker override rollback failed; retrying");
			worker_restored = mesh()->restore_overrides(old_slots, old_bricks, r,
					job.region_slot, table, old_table, old_entries);
		}
		if (!worker_restored)
			UtilityFunctions::printerr("VoxelWorld: worker override rollback could not be completed");
		// Newly acquired slots are never part of the old table. Release them even when a
		// rollback reports failure; the old table/op list remains authoritative and the
		// transaction is refused/retried rather than leaking capacity.
		for (const ve::IVec3 acquired : newly_acquired) store_->overrides()->release(acquired);
		return render_restored && worker_restored;
	};
	if (!render_ok) {
		rollback_publication();
		return refuse();
	}
	if (!mesh()->publish_overrides(slots, results[0].baked, r, job.region_slot, table, entries)) {
		if (!rollback_publication()) return refuse();
		return refuse();
	}
	for (size_t i = 0; i < slots.size(); i++) *store_->overrides()->data(slots[i]) = results[0].baked[i];
	store_->edit_log()->clear_region(r);
	const ve::IVec3 hi_brick{base.x + ve::kRegionBricks - 1,
			base.y + ve::kRegionBricks - 1, base.z + ve::kRegionBricks - 1};
	pending_dirty().push_back({ve::chunk_of_brick(base), ve::chunk_of_brick(hi_brick)});
	float lo[3], first_hi[3], last_lo[3], hi[3];
	ve::brick_world_aabb(base, lo, first_hi);
	ve::brick_world_aabb({base.x + ve::kRegionBricks - 1, base.y + ve::kRegionBricks - 1,
			base.z + ve::kRegionBricks - 1}, last_lo, hi);
	if (lod_tree()) {
		std::lock_guard<std::mutex> lock(lod_mutex());
		lod_tree()->mark_dirty(lo, hi);
	}
	if (streamer()) streamer()->queue_region_regeneration_locked(r);
	store_->override_tables()[key] = table;
	return true;
}

} // namespace godot
