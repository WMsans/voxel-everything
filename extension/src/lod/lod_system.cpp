#include "lod/lod_system.h"

#include "core/world_store.h"
#include "generator/edit_ops.h" // ve::op_world_aabb (note_edit fan-out)
#include "lod/lod_arena.h"     // ve::lod_pages_for_quads
#include "lod/lod_contour.h"   // ve::kLodQuadsPerPage
#include "lod/lod_grid.h"      // ve::lod_chunk_aabb / lod_cell_size / kLodFadeStartM
#include "render/lod_build_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "render/mesh_service.h"
#include "render/orchestrator.h"
#include "render/sun_shadow_pass.h" // mark_dirty() on page-set changes (orchestrator.h only forward-declares)
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>

namespace godot {

LodSystem::LodSystem(Collaborators handles) : handles_(handles) {}

// Moved verbatim from VoxelWorld::gather_lod_ops (Task 15); the WorldStore accesses are
// already through its public API, unchanged.
void LodSystem::gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return;
	out->clear();
	std::lock_guard<std::mutex> lock(store()->edit_mutex());
	if (!store()->edit_log()) return;
	float lo[3], hi[3];
	ve::lod_chunk_aabb(level, coord, lo, hi);
	const float pad = std::max(2.0f * ve::lod_cell_size(level), ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, out);
	// M4 errata 1: the flattened cross-region list can exceed the cap. A chronological
	// prefix is a valid world state; a suffix could apply an add without the subtract that
	// made room for it.
	if (out->size() > ve::kMaxRegionOps) out->resize(ve::kMaxRegionOps);
}

ve::WorldBounds LodSystem::world_bounds() const {
	return ve::world_bounds(store()->config());
}

void LodSystem::ensure_lod() {
	if (lod_tree_ && lod_pool_ && lod_pool_->page_count() > 0) return;
	handles_.ensure_initialized_thunk(handles_.ensure_initialized_self);
	RenderingDevice *device = render()->rd();
	if (!device) return;
	if (!lod_tree_) {
		ve::LodTreeConfig cfg;
		cfg.bounds = world_bounds();
		lod_tree_ = new ve::LodTree(cfg);
	}
	if (!lod_pool_) lod_pool_ = new LodPool();
	if (lod_pool_->page_count() == 0 && !lod_pool_->initialize(device, max_lod_pages_))
		UtilityFunctions::printerr("VoxelWorld: LodPool initialize failed");
}

void LodSystem::fade_band(float *fade_start, float *fade_end) const {
	// With the near field forced off the far field owns every distance: move the seam to
	// zero and make the fade span essentially infinite so the LoD build gate requests the
	// near chunks and the fragment shader keeps every far-field fragment.
	if (!handles_.near_field_enabled->load(std::memory_order_relaxed)) {
		if (fade_start) *fade_start = 0.0f;
		if (fade_end) *fade_end = 1.0e9f;
		return;
	}
	// Until the streamer has run a frame there is nothing measured, and before the first
	// regions land the measurement is "complete out to 0 m" -- both would swing the seam.
	// Fall back to the CONFIGURED radius there: the seam then starts where the near field
	// intends to reach and only tightens if the atlas cannot fund it, instead of jumping
	// once streaming begins and stranding the chunks the walk built under the old band.
	float reach = store()->residency() ? store()->residency()->complete_radius_m() : 0.0f;
	if (reach <= 0.0f) reach = store()->config().residency_radius_m;
	ve::lod_fade_band(reach, fade_start, fade_end);
}

void LodSystem::tick(const ve::LodCamera &cam, const ve::LodOcclusion *occ) {
	using LodKey = ve::LodKey;
	std::unique_lock<std::mutex> lock(lod_mutex_);
	ensure_lod();
	if (!lod_tree_ || !lod_pool_) return;
	// The gate that decides which chunks are worth building has to agree with the fragment
	// shader about where the far field starts, or it refuses to build exactly the chunks the
	// near field can no longer cover.
	{
		float fs = ve::kLodFadeStartM;
		fade_band(&fs, nullptr);
		lod_tree_->set_fade_start_m(fs);
	}
	lod_tree_->walk(cam, occ, ++lod_frame_, &lod_walk_);

	// Results first: a page that arrives this frame should be drawable this frame.
	std::vector<LodBuildResult> done;
	if (mesh() && mesh()->collect_lod(&done) > 0) {
		for (LodBuildResult &r : done) {
			if (r.failed) {
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				const auto old_it = lod_pages_of_.find(key);
				if (old_it != lod_pages_of_.end()) {
					// Stale beats missing: a failed rebuild keeps the old pages drawable and
					// is re-affirmed Ready-with-dirty so the next walk retries it. Do not
					// release the old pages and do not mark the node failed (that would
					// un-draw it).
					lod_tree_->note_ready_dirty(r.level, r.coord);
					lod_pressure_ += ve::lod_pages_for_quads(int(r.quads.size()));
				} else {
					lod_tree_->note_failed(r.level, r.coord);
				}
				continue;
			}
			if (r.overflow) {
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				if (lod_overflow_logged_.insert(key).second)
					UtilityFunctions::printerr("VoxelWorld: LoD chunk (level ", r.level,
							", ", r.coord.x, ", ", r.coord.y, ", ", r.coord.z,
							") overflowed; keeping first ", ve::kLodMaxQuadsPerChunk,
							" quads");
			}
			if (r.quads.empty()) {
				// Empty result. If an edit landed while this build was in flight, the result
				// is stale: keep any old pages drawing (stale beats missing) or leave a
				// non-resident node requestable. Only a non-dirty empty result is terminal,
				// and only then may the old GPU pages be released.
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				const bool dirty = lod_tree_->is_dirty(r.level, r.coord);
				const auto old_it = lod_pages_of_.find(key);
				if (dirty) {
					if (old_it != lod_pages_of_.end()) {
						// Old pages stay drawable; note_ready_dirty re-requests the rebuild.
						lod_tree_->note_ready_dirty(r.level, r.coord);
					} else {
						// Nothing to keep drawing; note_empty leaves the node requestable.
						lod_tree_->note_empty(r.level, r.coord);
					}
					continue;
				}
				// Genuinely empty: release any old pages before telling the tree, otherwise
				// the tree stops drawing/requesting it while the stale GPU pages stay
				// allocated forever.
				if (old_it != lod_pages_of_.end()) {
					for (int p : old_it->second) lod_page_quads_.erase(p);
					lod_pool_->release(old_it->second);
					if (render()->sun_shadow_pass()) render()->sun_shadow_pass()->mark_dirty();
					lod_pages_of_.erase(old_it);
				}
				lod_tree_->note_empty(r.level, r.coord);
				continue;
			}
			std::vector<int> pages;
			if (!lod_pool_->upload(r.level, r.coord, r.quads, &pages)) {
				// Refused, not half-funded. If the chunk already has resident pages, keep
				// drawing them: stale beats missing. Re-affirm Ready-with-dirty using the old
				// page list so the node stays drawable AND is re-requested next frame; a node
				// with no old pages still fails and is re-requested next frame.
				const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
				const auto old_it = lod_pages_of_.find(key);
				if (old_it != lod_pages_of_.end()) {
					lod_tree_->note_ready_dirty(r.level, r.coord);
					// Keep the old page list in lod_pages_of_: it remains the node's drawable
					// pages until a later upload succeeds and replaces them.
				} else {
					lod_tree_->note_failed(r.level, r.coord);
				}
				// Accumulate across refusals in this frame so evictions recover enough pages
				// for every refused rebuild, not just the last one.
				lod_pressure_ += ve::lod_pages_for_quads(int(r.quads.size()));
				continue;
			}
			// A rebuild replaces the old page list. Release the stale pages only once the
			// new pages are allocated and uploaded, so a refused rebuild keeps the old pages
			// drawing; after this point the tree points at the new list.
			if (render()->sun_shadow_pass()) render()->sun_shadow_pass()->mark_dirty();
			const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
			const auto old_it = lod_pages_of_.find(key);
			if (old_it != lod_pages_of_.end()) {
				for (int p : old_it->second) lod_page_quads_.erase(p);
				lod_pool_->release(old_it->second);
				lod_pages_of_.erase(old_it);
			}
			for (int i = 0; i < int(pages.size()); i++) {
				const int first = i * ve::kLodQuadsPerPage;
				const int count = std::min(ve::kLodQuadsPerPage,
						static_cast<int>(r.quads.size()) - first);
				lod_page_quads_[pages[static_cast<size_t>(i)]] = count;
			}
			lod_tree_->note_ready(r.level, r.coord, pages.front(), int(pages.size()));
			lod_pages_of_[key] = std::move(pages);
		}
	}

	// Then evictions, so the budget below sees the pages they returned.
	std::vector<ve::LodDrawItem> evicted;
	lod_tree_->collect_evictions(lod_frame_, lod_pressure_, &evicted);
	lod_pressure_ = 0;
	for (const ve::LodDrawItem &e : evicted) {
		const LodKey key{e.level, e.coord.x, e.coord.y, e.coord.z};
		const auto it = lod_pages_of_.find(key);
		if (it == lod_pages_of_.end()) continue;
		for (int p : it->second) lod_page_quads_.erase(p);
		lod_pool_->release(it->second);
		if (render()->sun_shadow_pass()) render()->sun_shadow_pass()->mark_dirty();
		lod_pages_of_.erase(it);
	}

	// Then this frame's builds, priority order, one batch. Mark the nodes building while
	// still holding lod_mutex_ so note_building's dirty-clear happens at submission time.
	// gather_ops takes edit_mutex_, so it must run AFTER releasing lod_mutex_ (lock
	// order: edit_mutex -> LodSystem::mutex()); the building flag prevents a concurrent walk
	// from re-requesting these nodes during that window, and a refused submit rolls the
	// flags back.
	std::vector<ve::LodBuildRequest> batch_requests;
	if (mesh() && !mesh()->lod_busy()) {
		// MeshService's LodBuildPass currently supports at most 8 LoD jobs per batch.
		// lod_builds_per_frame_ is user-facing and may be higher; submit_lod would reject
		// anything above the mesher's cap, so clamp the actual batch take here.
		const int take = std::min<int>({lod_builds_per_frame_, int(lod_walk_.requests.size()), 8});
		batch_requests.assign(lod_walk_.requests.begin(), lod_walk_.requests.begin() + take);
		for (const ve::LodBuildRequest &q : batch_requests)
			lod_tree_->note_building(q.level, q.coord);
	}
	lock.unlock();

	if (!batch_requests.empty()) {
		std::vector<LodBuildJob> batch;
		batch.reserve(batch_requests.size());
		for (const ve::LodBuildRequest &q : batch_requests) {
			LodBuildJob j;
			j.level = q.level;
			j.coord = q.coord;
			gather_ops(q.level, q.coord, &j.ops);
			batch.push_back(std::move(j));
		}
		if (!mesh()->submit_lod(std::move(batch))) {
			lock.lock();
			for (const ve::LodBuildRequest &q : batch_requests) {
				const LodKey key{q.level, q.coord.x, q.coord.y, q.coord.z};
				if (lod_pages_of_.find(key) != lod_pages_of_.end()) {
					lod_tree_->note_ready_dirty(q.level, q.coord);
				} else {
					lod_tree_->note_failed(q.level, q.coord);
				}
			}
			lock.unlock();
		}
	}

	lock.lock();
	prepare_raster_locked();
}

void LodSystem::prepare_raster() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	prepare_raster_locked();
}

// Every resident page casts, COARSEST LEVEL FIRST.
//
// The map must not use the camera's cut: that is frustum culled, so terrain beside or behind
// the camera would stop shadowing. But residency is a cache, not a partition -- a coarse
// ancestor stays resident while its own finer children do, and both describe the same ground.
// Rasterized together with a nearest-to-the-sun depth test, the ancestor's tent-filtered
// surface wins: it bulges metres above the one the camera draws (measured up to 7 m), and the
// far field is then shadowed by a silhouette that is on no screen.
//
// Order is what resolves it. SunShadowPass writes depth unconditionally, so within one texel
// the LAST page drawn wins; emitting coarse before fine means the finest description of a
// piece of ground always overwrites the coarser ones, while ground that only a coarse page
// describes still casts. std::map orders LodKey by level ascending, so this walks it backwards.
void LodSystem::prepare_shadow_raster() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!render()->lod_raster_pass() || !lod_pool_) return;
	std::vector<ve::LodPageDraw> page_draws;
	ve::lod_collect_shadow_page_draws(lod_pages_of_, lod_page_quads_, &page_draws);
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(page_draws.size());
	for (const ve::LodPageDraw &pd : page_draws)
		pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
	render()->lod_raster_pass()->set_draw_pages(pages);
}

void LodSystem::prepare_raster_locked() {
	if (!render()->lod_raster_pass() || !lod_pool_) return;
	std::vector<ve::LodPageDraw> page_draws;
	ve::lod_collect_page_draws(lod_walk_.draws, lod_pages_of_, lod_page_quads_, &page_draws);
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(page_draws.size());
	for (const ve::LodPageDraw &pd : page_draws)
		pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
	render()->lod_raster_pass()->set_draw_pages(pages);
}

// The LoD tail of VoxelWorld::append_edit_locked, moved verbatim (Task 15): caller holds
// edit_mutex(); the lod_mutex_ acquisition site travels with the code.
void LodSystem::note_edit(const ve::EditOp &op) {
	if (!lod_tree_) return;
	float lo[3], hi[3];
	ve::op_world_aabb(op, lo, hi);
	// Every level: ve::LodTree::mark_dirty walks them itself, and the relevance cut is
	// at the HALF-CELL supersample resolution rather than the cell -- a 5 m crater still
	// registers at L4's 6.4 m cells, which is the point of the reduction change. Only
	// ops shorter than half a cell on every axis are genuinely unrepresentable.
	// Lock order: caller holds edit_mutex(), tick never holds lod_mutex_ while taking
	// edit_mutex(), so edit_mutex -> lod_mutex_ is safe.
	std::lock_guard<std::mutex> lock(lod_mutex_);
	lod_tree_->mark_dirty(lo, hi);
}

// The _exit_tree() LoD half, verbatim statement-for-statement (Task 15).
void LodSystem::teardown() {
	if (lod_pool_) {
		delete lod_pool_;
		lod_pool_ = nullptr;
	}
	if (lod_tree_) {
		delete lod_tree_;
		lod_tree_ = nullptr;
	}
	lod_pages_of_.clear();
	lod_page_quads_.clear();
}

} // namespace godot
