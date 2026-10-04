#include "lod/lod_system.h"

#include "core/world_store.h"
#include "generator/edit_ops.h" // ve::op_world_aabb (edit fan-out)
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

LodStats LodSystem::stats() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	ensure_lod();
	LodStats s;
	if (lod_pool_) {
		s.pages_total = lod_pool_->page_count();
		s.pages_free = lod_pool_->free_pages();
		s.chunk_records = lod_pool_->chunk_record_count();
		s.chunk_records_used = lod_pool_->chunk_records_used();
		s.chunk_records_high_water = lod_pool_->chunk_records_high_water();
		s.pages_high_water = lod_pool_->pages_high_water();
		s.budget_bound = lod_pool_->budget_bound();
	}
	s.chunks_resident = static_cast<int>(lod_pages_of_.size());
	if (lod_tree_) lod_tree_->dirty_stats(&s.dirty_chunks, &s.dirty_levels);
	for (const ve::LodDrawItem &item : lod_walk_.draws) s.draw_pages += item.page_count;
	s.draw_min_level = ve::kLodLevels;
	for (const ve::LodDrawItem &item : lod_walk_.draws)
		s.draw_min_level = std::min(s.draw_min_level, item.level);
	for (const auto &kv : lod_pages_of_)
		if (kv.first.level < 0) s.fine_pages += static_cast<int>(kv.second.size());
	// The exact page identities of the current camera cut, not just their count: a bounded
	// pool may keep a drawable coarse cut while refinement requests remain pending.
	std::vector<ve::LodPageDraw> draw_page_list;
	ve::lod_collect_page_draws(lod_walk_.draws, lod_pages_of_, lod_page_quads_, &draw_page_list);
	for (const ve::LodPageDraw &page : draw_page_list) s.draw_page_ids.push_back(page.page);
	for (const auto &page : lod_page_quads_)
		if (page.second > 0) s.resident_page_ids.push_back(page.first);
	s.requests = lod_walk_.requests;
	// LodArena::alloc is all-or-nothing, so this should always be zero -- but a hardcoded 0
	// would make the test that asserts it vacuous. MEASURE the two shapes a partially funded
	// build would take: a chunk holding a page the per-page quad count never learned about,
	// and arena pages that no resident chunk owns.
	int partial = 0;
	size_t owned_pages = 0;
	for (const auto &kv : lod_pages_of_) {
		owned_pages += kv.second.size();
		for (int p : kv.second) {
			if (lod_page_quads_.find(p) == lod_page_quads_.end()) {
				partial++;
				break;
			}
		}
	}
	s.shell_chunks = shell_grid_.size();
	// Shell pages are owned by a shell chunk exactly as terrain pages are owned by a tree
	// chunk, so they count here too: without this the unowned-pages term below would report
	// every shell page as a leak.
	for (const auto &kv : shell_pages_of_) {
		owned_pages += kv.second.size();
		s.shell_pages += static_cast<int>(kv.second.size());
	}
	// Island shell pages are owned by an atlas slot exactly as terrain pages are owned by a
	// tree chunk, so they count here for the same reason.
	for (const auto &kv : island_shell_pages_) owned_pages += kv.second.size();
	for (const auto &kv : island_mesh_pages_) owned_pages += kv.second.size();
	const int unowned = (s.pages_total - s.pages_free) - static_cast<int>(owned_pages);
	s.partial_allocations = partial + (unowned > 0 ? unowned : 0);
	s.op_overflow = lod_op_overflow_;
	return s;
}

// Moved verbatim from VoxelWorld::gather_lod_ops (Task 15); the WorldStore accesses are
// already through its public API, unchanged.
bool LodSystem::gather_ops(int level, ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return false;
	out->clear();
	std::lock_guard<std::mutex> lock(store()->edit_mutex());
	if (!store()->edit_log()) return true;
	float lo[3], hi[3];
	ve::lod_chunk_aabb(level, coord, lo, hi);
	const float pad = std::max(2.0f * ve::lod_cell_size(level), ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, out);
	// S3c: a chronological prefix used to be kept past the cap, silently dropping the newest
	// edits. Ops this level cannot represent go first; if the rest still does not fit, the
	// build is refused and the chunk keeps its last good pages.
	return ve::lod_cut_ops(level, out);
}

// Task 7: the near shell's op gather. gather_ops() with the shell's origin/cell and the raw
// cap -- there is no per-level cut for a chunk that is not on the LoD grid.
bool LodSystem::gather_shell_ops(ve::IVec3 coord, std::vector<ve::EditOp> *out) {
	if (!out) return false;
	out->clear();
	std::lock_guard<std::mutex> lock(store()->edit_mutex());
	if (!store()->edit_log()) return true;
	float lo[3], hi[3];
	ve::shell_chunk_aabb(coord, lo, hi);
	// ve::transparent_boxes() does NOT pad the override bricks it is handed, and the build
	// samples world state at kLatticeFilterPad outside the chunk, so an op just past the face
	// still tints a lattice cell inside it. Pad exactly as gather_ops() pads.
	const float pad = std::max(2.0f * ve::kShellCell, ve::kLatticeFilterPad);
	for (int a = 0; a < 3; a++) {
		lo[a] -= pad;
		hi[a] += pad;
	}
	ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, out);
	return out->size() <= static_cast<size_t>(ve::kMaxRegionOps);
}

// Assumes lod_mutex_ is held. The shell's page bookkeeping, split out of tick()'s result loop
// so a rebuild and an eviction both release through ONE path.
void LodSystem::release_shell_pages_locked(ve::IVec3 coord) {
	const ve::LodKey key{ve::kShellLevel, coord.x, coord.y, coord.z};
	const auto it = shell_pages_of_.find(key);
	if (it == shell_pages_of_.end()) return;
	for (int p : it->second) lod_page_quads_.erase(p);
	lod_pool_->release(it->second);
	shell_pages_of_.erase(it);
}

// An island's transparent shell becomes ordinary LoD pool pages: chunk-record flag bits 8..
// carry `atlas_slot + 1`, which is what shaders/shell.vert.glsl reads to place the quads in
// the island's LOCAL space through its descriptor. So an island costs no draw call of its own,
// and the pages are released the moment the slot leaves the live mask -- which is read from the
// descriptor array itself, so a slot killed by clear_slot() drops its pages with it.
void LodSystem::apply_island_shells(std::vector<IslandShell> shells, uint32_t live_mask) {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!lod_pool_ || lod_pool_->page_count() == 0) return;
	const auto release = [this](std::map<int, std::vector<int>> &owner, int slot) {
		const auto it = owner.find(slot);
		if (it == owner.end()) return;
		for (int p : it->second) lod_page_quads_.erase(p);
		lod_pool_->release(it->second);
		owner.erase(it);
	};
	for (std::map<int, std::vector<int>> *owner : {&island_shell_pages_, &island_mesh_pages_})
		for (auto it = owner->begin(); it != owner->end();) {
			const int slot = (it++)->first;
			if (slot < 0 || slot >= 32 || (live_mask & (1u << slot)) == 0u) release(*owner, slot);
		}
	// ponytail: a refused upload (pool/record exhaustion) silently drops this block of the
	// island. Count it per island if that ever shows up as an island with a missing piece.
	const auto upload = [this](const IslandShell &s, const std::vector<ve::IslandShellBlock> &blocks,
								uint32_t flags) {
		std::vector<int> all;
		for (const ve::IslandShellBlock &b : blocks) {
			std::vector<int> pages;
			if (!lod_pool_->upload_at(b.origin_local, s.voxel, 0u, flags, b.quads, b.normals,
						&pages))
				continue; // pool full: this block of the island is not drawn
			for (size_t i = 0; i < pages.size(); i++) {
				const int first = static_cast<int>(i) * ve::kLodQuadsPerPage;
				lod_page_quads_[pages[i]] = std::min(ve::kLodQuadsPerPage,
						static_cast<int>(b.quads.size()) - first);
			}
			all.insert(all.end(), pages.begin(), pages.end());
		}
		return all;
	};
	for (IslandShell &s : shells) {
		if (s.atlas_slot < 0 || s.atlas_slot >= 32) continue;
		// A re-extracted island replaces both of its meshes.
		release(island_shell_pages_, s.atlas_slot);
		release(island_mesh_pages_, s.atlas_slot);
		std::vector<int> shell_pages = upload(s, s.blocks, ve::island_shell_flags(s.atlas_slot));
		if (!shell_pages.empty()) island_shell_pages_[s.atlas_slot] = std::move(shell_pages);
		std::vector<int> mesh_pages = upload(s, s.opaque, ve::island_mesh_flags(s.atlas_slot));
		if (!mesh_pages.empty()) island_mesh_pages_[s.atlas_slot] = std::move(mesh_pages);
	}
}

// Called with lod_mutex_ held through `lock`; releases it across the edit-lock section
// (lock order: edit_mutex -> lod mutex) and re-takes it before returning. Leaves `lock` HELD
// on every path, which is what tick() continues on.
void LodSystem::refresh_shell_candidates(const ve::LodCamera &cam,
		std::unique_lock<std::mutex> &lock) {
	// With the near field off there is no near shell to draw at all -- the far field draws
	// transparent solid itself -- and fade_band()'s near-field-off branch reports fade_end
	// = 0. A radius derived from it is meaningless, and the 1e9 it used to be was a landmine:
	// collect_ops_for_aabb caps its region span at 128, so it would silently return NO ops
	// (measured: no shell at all, and no error), while transparent_boxes still walks every
	// override brick in the store on every recompute. Gate the shell on the near field rather
	// than feed it a clamped radius.
	const bool enabled = render()->transparency_settings().enabled &&
			render()->near_field_enabled();
	const ve::IVec3 cam_chunk = ve::shell_chunk_of_point(cam.pos[0], cam.pos[1], cam.pos[2]);
	const bool moved = cam_chunk.x != shell_cam_chunk_.x || cam_chunk.y != shell_cam_chunk_.y ||
			cam_chunk.z != shell_cam_chunk_.z;
	// A runtime toggle with a parked camera and no edit changes neither chunk nor dirty, so
	// without this the toggle would not be observed at all: off would keep the candidate set
	// and its Unknown chunks, on would wait for the camera to walk into another shell chunk.
	const bool toggled = enabled != shell_enabled_;
	float fade_end = ve::kLodFadeEndM;
	fade_band(nullptr, &fade_end);
	// One shell chunk of reach past the far field's edge -- margin, NOT seam reach.
	// shell_candidates culls on the NEAREST point of a chunk's AABB, so a chunk straddling
	// fade_end is already a candidate at radius = fade_end and the extra chunk buys no seam
	// coverage. What it does buy is one shell chunk of build slots and pool pages that are
	// never rasterised: past fade_end every fragment of a near page is dithered away (the
	// fragment test is per-pixel, not per-chunk). Kept because the fade boundary is evaluated
	// per fragment in float on the GPU and candidacy on the CPU, so a margin is cheap
	// insurance against the two disagreeing; drop it to fade_end if a profile ever charges
	// for the wasted pages.
	const float radius = fade_end + ve::kShellChunkSize;
	// ponytail: a recompute rescans the whole edit log and every override brick in range, and
	// it now runs on the TICK path -- once per dirty tick and once per 3.2 m camera crossing.
	// Cache the box set per brick (or recompute over only the delta's dirty regions) if a
	// profile shows it; the correct-by-construction full scan is what buys the goldens.
	lock.unlock();
	std::vector<ve::ShellBox> boxes;
	bool recompute = moved || toggled;
	{
		std::lock_guard<std::mutex> edit_lock(store()->edit_mutex());
		recompute = recompute || shell_dirty_;
		shell_dirty_ = false;
		if (recompute && enabled && store()->edit_log()) {
			// Padded like gather_shell_ops(), for the same reason: ve::transparent_boxes()
			// applies [lo, hi] to the override bricks WITHOUT padding and trusts the caller
			// for the ops. An op sitting just outside the nominal radius still tints a
			// lattice cell inside it, so it must reach the collector.
			const float pad = ve::kLatticeFilterPad;
			float lo[3], hi[3];
			for (int a = 0; a < 3; a++) {
				lo[a] = cam.pos[a] - radius - pad;
				hi[a] = cam.pos[a] + radius + pad;
			}
			std::vector<ve::EditOp> ops;
			ve::collect_ops_for_aabb(*store()->edit_log(), lo, hi, &ops);
			ve::transparent_boxes(ops.data(), static_cast<int>(ops.size()), &store()->volumes(),
					store()->overrides(), lo, hi, &boxes);
		}
	}
	lock.lock();
	if (!recompute) return;
	shell_cam_chunk_ = cam_chunk;
	shell_enabled_ = enabled;
	std::vector<ve::IVec3> chunks, evicted;
	// Off: the candidate set is empty, so set_candidates evicts everything and no shell job
	// is ever requested.
	if (enabled) ve::shell_candidates(boxes, cam.pos, radius, &chunks);
	shell_grid_.set_candidates(chunks, &evicted);
	for (ve::IVec3 c : evicted) release_shell_pages_locked(c);
}

void LodSystem::ensure_lod() {
	if (lod_tree_ && lod_pool_ && lod_pool_->page_count() > 0) return;
	handles_.ensure_initialized_thunk(handles_.ensure_initialized_self);
	RenderingDevice *device = render()->rd();
	if (!device) return;
	if (!lod_tree_) {
		ve::LodTreeConfig cfg;
		cfg.stream_radius_m = store()->config().stream_radius_m;
		lod_tree_ = new ve::LodTree(cfg);
	}
	if (!lod_pool_) lod_pool_ = new LodPool();
	if (lod_pool_->page_count() == 0 &&
			!lod_pool_->initialize(device, max_lod_pages_, max_lod_chunk_records_))
		UtilityFunctions::printerr("VoxelWorld: LodPool initialize failed");
}

void LodSystem::fade_band(float *fade_start, float *fade_end) const {
	// With the near field off the far field owns every distance: start and end both at 0
	// put every fragment at t = 1, past every bayer4 threshold, in lod.frag.glsl, the
	// composite, the shell passes and deferred's far_field_owns alike. (0 / 1e9 put t at
	// d / 1e9 instead, which kept one pixel in sixteen.) The LoD build gate reads the same 0,
	// so it requests the near chunks.
	if (!render()->near_field_enabled()) {
		if (fade_start) *fade_start = 0.0f;
		if (fade_end) *fade_end = 0.0f;
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
	// Edits queued since the last tick, applied before the walk decides what to build. Takes
	// the edit lock and must therefore run before this tick takes lod_mutex_.
	drain_invalidations();
	using LodKey = ve::LodKey;
	std::unique_lock<std::mutex> lock(lod_mutex_);
	// Recorded before the early-outs: the sun ortho needs the camera whether or not this
	// tick had a tree to walk, and a stale centre would drag the shadow map behind the view.
	for (int a = 0; a < 3; a++) last_cam_[a] = cam.pos[a];
	has_last_cam_ = true;
	lod_shadow_cam_ = cam;
	ensure_lod();
	if (!lod_tree_ || !lod_pool_) return;
	// The gate that decides which chunks are worth building has to agree with the fragment
	// shader about where the far field starts, or it refuses to build exactly the chunks the
	// near field can no longer cover.
	{
		float fs = ve::kLodFadeStartM;
		fade_band(&fs, nullptr);
		lod_tree_->set_fade_start_m(fs);
		// Raster mode lets the walk descend to 0.1 m (spec 2026-10-04 §3). Switching back
		// leaves the fine nodes unvisited, so they age out through collect_evictions.
		lod_tree_->set_min_level(render()->raster_mode() ? ve::kLodMinLevel : 0);
	}
	lod_tree_->walk(cam, occ, ++lod_frame_, &lod_walk_);

	// Results first: a page that arrives this frame should be drawable this frame.
	std::vector<LodBuildResult> done;
	if (mesh() && mesh()->collect_lod(&done) > 0) {
		for (LodBuildResult &r : done) {
			// Task 7: a shell result NEVER touches the tree -- it has no node there. First
			// statement in the loop, so no shell chunk can leak into a far-field node's
			// page list or its dirty bookkeeping.
			if (r.shell_only) {
				if (r.failed) {
					shell_grid_.note_failed(r.coord);
					continue;
				}
				if (r.quads.empty()) {
					release_shell_pages_locked(r.coord);
					shell_grid_.note_result(r.coord, false);
					continue;
				}
				float origin[3];
				ve::shell_chunk_origin(r.coord, origin);
				std::vector<int> pages;
				if (!lod_pool_->upload_at(origin, ve::kShellCell, 0u, 1u, r.quads, r.normals, &pages)) {
					// Refused: the old pages (if any) keep drawing and the chunk is asked for
					// again -- stale beats missing, as for terrain chunks.
					shell_grid_.note_failed(r.coord);
					// As for a refused terrain upload: the pages this build wanted are pressure.
					// Without it a permanently unfundable shell chunk burns a build slot every
					// frame forever, and shell jobs take the cap first, so the far field starves.
					lod_pressure_ += ve::lod_pages_for_quads(int(r.quads.size()));
					continue;
				}
				release_shell_pages_locked(r.coord);
				for (int i = 0; i < int(pages.size()); i++) {
					const int first = i * ve::kLodQuadsPerPage;
					lod_page_quads_[pages[static_cast<size_t>(i)]] = std::min(ve::kLodQuadsPerPage,
							static_cast<int>(r.quads.size()) - first);
				}
				shell_pages_of_[ve::LodKey{ve::kShellLevel, r.coord.x, r.coord.y, r.coord.z}] =
						std::move(pages);
				shell_grid_.note_result(r.coord, true);
				continue;
			}
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
					for (int p : old_it->second) {
						lod_page_quads_.erase(p);
						lod_transparent_pages_.erase(p);
					}
					lod_pool_->release(old_it->second);
					if (render()->passes().sun_shadow) render()->passes().sun_shadow->mark_dirty();
					lod_pages_of_.erase(old_it);
				}
				lod_tree_->note_empty(r.level, r.coord);
				continue;
			}
			std::vector<int> pages;
			if (!lod_pool_->upload(r.level, r.coord, r.quads, r.normals, &pages)) {
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
			if (render()->passes().sun_shadow) render()->passes().sun_shadow->mark_dirty();
			const LodKey key{r.level, r.coord.x, r.coord.y, r.coord.z};
			const auto old_it = lod_pages_of_.find(key);
			if (old_it != lod_pages_of_.end()) {
				for (int p : old_it->second) {
					lod_page_quads_.erase(p);
					lod_transparent_pages_.erase(p);
				}
				lod_pool_->release(old_it->second);
				lod_pages_of_.erase(old_it);
			}
			for (int i = 0; i < int(pages.size()); i++) {
				const int first = i * ve::kLodQuadsPerPage;
				const int count = std::min(ve::kLodQuadsPerPage,
						static_cast<int>(r.quads.size()) - first);
				lod_page_quads_[pages[static_cast<size_t>(i)]] = count;
				if (ve::lod_quads_have_transparent(r.quads.data() + first, count))
					lod_transparent_pages_.insert(pages[static_cast<size_t>(i)]);
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
		for (int p : it->second) {
			lod_page_quads_.erase(p);
			lod_transparent_pages_.erase(p);
		}
		lod_pool_->release(it->second);
		if (render()->passes().sun_shadow) render()->passes().sun_shadow->mark_dirty();
		lod_pages_of_.erase(it);
	}

	// Then the shell's candidate set: recomputed only when an edit landed, the enabled flag
	// toggled, or the camera entered another shell chunk, and only under the edit lock --
	// released from `lock` first, exactly like gather_ops() below.
	refresh_shell_candidates(cam, lock);

	// Then this frame's builds, priority order, one batch. Mark the nodes building while
	// still holding lod_mutex_ so note_building's dirty-clear happens at submission time.
	// gather_ops takes edit_mutex_, so it must run AFTER releasing lod_mutex_ (lock
	// order: edit_mutex -> LodSystem::mutex()); the building flag prevents a concurrent walk
	// from re-requesting these nodes during that window, and a refused submit rolls the
	// flags back.
	std::vector<ve::LodBuildRequest> batch_requests;
	std::vector<ve::IVec3> shell_requests;
	if (mesh() && !mesh()->lod_busy()) {
		const int cap = std::min<int>(lod_builds_per_frame_, mesh()->lod_max_jobs());
		// Shell chunks first: a missing shell is transparent solid that is not there at all, a
		// missing far chunk is a coarser horizon. Skipped entirely while transparency is off --
		// refresh_shell_candidates already empties the candidate set on the toggle, so this is
		// defence in depth, and it must not gate the far-field submissions below.
		if (render()->transparency_settings().enabled) {
			shell_grid_.requests(cam.pos, cap, &shell_requests);
			// note_building clears the dirty flag, so an edit landing between requests() and here
			// is swallowed. Safe ONLY because the build samples world state AFTER that edit: the
			// shell it produces already contains it.
			for (ve::IVec3 c : shell_requests) {
				shell_grid_.note_building(c);
			}
		}
		const int take = std::min<int>(cap - int(shell_requests.size()),
				int(lod_walk_.requests.size()));
		batch_requests.assign(lod_walk_.requests.begin(), lod_walk_.requests.begin() + take);
		for (const ve::LodBuildRequest &q : batch_requests)
			lod_tree_->note_building(q.level, q.coord);
	}
	lock.unlock();

	if (!batch_requests.empty() || !shell_requests.empty()) {
		std::vector<LodBuildJob> batch;
		std::vector<ve::LodBuildRequest> submitted, refused;
		std::vector<ve::IVec3> shell_submitted, shell_refused;
		batch.reserve(batch_requests.size() + shell_requests.size());
		for (ve::IVec3 c : shell_requests) {
			LodBuildJob j;
			j.level = ve::kShellLevel;
			j.coord = c;
			j.shell_only = true;
			if (!gather_shell_ops(c, &j.ops)) {
				shell_refused.push_back(c);
				continue;
			}
			shell_submitted.push_back(c);
			batch.push_back(std::move(j));
		}
		for (const ve::LodBuildRequest &q : batch_requests) {
			LodBuildJob j;
			j.level = q.level;
			j.coord = q.coord;
			if (!gather_ops(q.level, q.coord, &j.ops)) {
				refused.push_back(q);
				continue;
			}
			submitted.push_back(q);
			batch.push_back(std::move(j));
		}
		if (!refused.empty() || !shell_refused.empty()) {
			lock.lock();
			for (const ve::LodBuildRequest &q : refused) lod_tree_->note_refused(q.level, q.coord);
			lod_op_overflow_ += static_cast<int>(refused.size());
			// ponytail: a shell chunk over the op cap is simply not drawn; it never retries
			// until something dirties it. Consolidation brings the region back under the cap.
			for (ve::IVec3 c : shell_refused) shell_grid_.note_result(c, false);
			lock.unlock();
		}
		if (!batch.empty() && !mesh()->submit_lod(std::move(batch))) {
			lock.lock();
			for (const ve::LodBuildRequest &q : submitted) {
				const LodKey key{q.level, q.coord.x, q.coord.y, q.coord.z};
				if (lod_pages_of_.find(key) != lod_pages_of_.end()) {
					lod_tree_->note_ready_dirty(q.level, q.coord);
				} else {
					lod_tree_->note_failed(q.level, q.coord);
				}
			}
			for (ve::IVec3 c : shell_submitted) shell_grid_.note_failed(c);
			lock.unlock();
		}
	}

	lock.lock();
	prepare_raster_locked();
}

bool LodSystem::last_camera(float out[3]) const {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!has_last_cam_) return false;
	for (int a = 0; a < 3; a++) out[a] = last_cam_[a];
	return true;
}

void LodSystem::prepare_raster() {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	prepare_raster_locked();
}

// The sun's cut: ONE description of each piece of ground, the level the camera walk chose,
// everywhere in the world rather than only inside the frustum (see shadow_cut() below).
//
// It used to be every RESIDENT page instead, because the camera's own list is frustum culled
// and terrain beside or behind the camera has to keep casting. But residency is a cache, not
// a partition: a coarse ancestor stays resident while its own finer children do, and both
// describe the same ground. Rasterized into one map they disagree by metres -- levels 5 to 7
// are 12.8 to 51.2 m cells and are never evicted, so their tent-filtered surfaces arch over
// the entire world -- and whichever survived the depth test then reported an occluder 2 to
// 15 m above open sunlit ground. Ordering the draws coarsest-first and writing depth
// unconditionally only moved the problem: the finest page wins the texels it covers, and the
// coarse bulge keeps every texel it does not.
void LodSystem::prepare_shadow_raster(float radius, int min_level) {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	if (!render()->passes().lod_raster || !lod_pool_ || !lod_tree_) return;
	std::vector<ve::LodDrawItem> cut;
	lod_tree_->shadow_cut(lod_shadow_cam_, radius, min_level, &cut);
	std::vector<ve::LodPageDraw> page_draws;
	ve::lod_collect_page_draws(cut, lod_pages_of_, lod_page_quads_, &page_draws);
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(page_draws.size());
	for (const ve::LodPageDraw &pd : page_draws)
		pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
	render()->passes().lod_raster->set_draw_pages(pages);
}

void LodSystem::prepare_raster_locked() {
	if (!render()->passes().lod_raster || !lod_pool_) return;
	std::vector<ve::LodPageDraw> page_draws;
	ve::lod_collect_page_draws(lod_walk_.draws, lod_pages_of_, lod_page_quads_, &page_draws);
	std::vector<LodRasterPass::PageDraw> pages;
	pages.reserve(page_draws.size());
	for (const ve::LodPageDraw &pd : page_draws)
		pages.push_back(LodRasterPass::PageDraw{pd.page, pd.quad_count});
	// Raster mode draws islands from their opaque meshes (spec 2026-10-04 §5): local-space
	// pages lod.vert.glsl places through the island descriptor. Never part of the walk's cut.
	if (render()->raster_mode())
		for (const auto &entry : island_mesh_pages_)
			for (int p : entry.second) {
				const auto q = lod_page_quads_.find(p);
				if (q != lod_page_quads_.end())
					pages.push_back(LodRasterPass::PageDraw{p, q->second});
			}
	render()->passes().lod_raster->set_draw_pages(pages);
	// ponytail: the shell list is the CPU walk's pages, not HiZ-culled; painted ice is rare.
	// Cull it too if a scene ever holds much of it.
	transparent_draw_pages_.clear();
	for (const ve::LodPageDraw &pd : page_draws)
		if (lod_transparent_pages_.count(pd.page)) transparent_draw_pages_.push_back(pd);
	// Task 7: the shell's own cut -- the far field's shell pages first (they are the ones
	// inside the camera frustum, and the shell raster has no HiZ of its own), then every
	// resident near-shell page. Task 10 appends the island group.
	shell_draw_pages_ = transparent_draw_pages_;
	for (const auto &entry : shell_pages_of_)
		for (int p : entry.second) {
			const auto q = lod_page_quads_.find(p);
			if (q != lod_page_quads_.end()) shell_draw_pages_.push_back(ve::LodPageDraw{p, q->second});
		}
	// The island shells: pages whose chunk record names an island, so the shell's vertex
	// shader places them through that island's descriptor. No per-island draw call.
	for (const auto &entry : island_shell_pages_)
		for (int p : entry.second) {
			const auto q = lod_page_quads_.find(p);
			if (q != lod_page_quads_.end()) shell_draw_pages_.push_back(ve::LodPageDraw{p, q->second});
		}
}

std::vector<ve::LodPageDraw> LodSystem::transparent_draw_pages() const {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	return transparent_draw_pages_;
}

std::vector<ve::LodPageDraw> LodSystem::shell_draw_pages() const {
	std::lock_guard<std::mutex> lock(lod_mutex_);
	return shell_draw_pages_;
}

// The LoD half of the edit fan-out. Edit lock held: queue only, never the lod mutex
// (core/edit_pipeline.h).
void LodSystem::record(const ve::Invalidation &inv) {
	if (inv.reason == ve::InvalidationReason::kRejected) return;
	ve::merge_or_cap(&pending_marks_,
			ve::Box3<float>{{inv.lo[0], inv.lo[1], inv.lo[2]}, {inv.hi[0], inv.hi[1], inv.hi[2]}});
	// Edit lock held, so this write is safe without the lod mutex: tick() reads and clears
	// it under this same lock.
	shell_dirty_ = true;
}

void LodSystem::drain_invalidations() {
	std::vector<ve::Box3<float>> marks;
	{
		std::lock_guard<std::mutex> edit_lock(store()->edit_mutex());
		marks.swap(pending_marks_);
	}
	if (marks.empty()) return;
	std::lock_guard<std::mutex> lock(lod_mutex_);
	// The shell's chunks overlap the edit's own box plus two cells of margin (Task 5's
	// mark_dirty), and they are NOT on the LoD grid, so LodTree::mark_dirty would miss them.
	for (const ve::Box3<float> &m : marks) shell_grid_.mark_dirty(m.lo, m.hi);
	// No tree yet: the marks predate it and the tree this tick builds reads the current world
	// anyway, exactly as the synchronous mark's `if (!lod_tree_) return` dropped them.
	if (!lod_tree_) return;
	// Every level: ve::LodTree::mark_dirty walks them itself, and the relevance cut is at the
	// HALF-CELL supersample resolution rather than the cell.
	for (const ve::Box3<float> &m : marks) lod_tree_->mark_dirty(m.lo, m.hi);
}

// The _exit_tree() LoD half, verbatim statement-for-statement (Task 15).
void LodSystem::teardown() {
	{
		std::lock_guard<std::mutex> edit_lock(store()->edit_mutex());
		pending_marks_.clear();
	}
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
	lod_transparent_pages_.clear();
	transparent_draw_pages_.clear();
	shell_grid_.clear();
	shell_pages_of_.clear();
	island_shell_pages_.clear();
	shell_draw_pages_.clear();
	shell_cam_chunk_ = ve::IVec3{INT32_MAX, 0, 0};
	shell_enabled_ = false;
	lod_op_overflow_ = 0;
}

void LodSystem::release_gpu() {
	{
		std::lock_guard<std::mutex> edit_lock(store()->edit_mutex());
		pending_marks_.clear();
	}
	if (lod_pool_) lod_pool_->teardown();
	if (lod_tree_) lod_tree_->clear();
	lod_pages_of_.clear();
	lod_page_quads_.clear();
	lod_transparent_pages_.clear();
	transparent_draw_pages_.clear();
	shell_grid_.clear();
	shell_pages_of_.clear();
	island_shell_pages_.clear();
	shell_draw_pages_.clear();
	shell_cam_chunk_ = ve::IVec3{INT32_MAX, 0, 0};
	shell_enabled_ = false;
	lod_overflow_logged_.clear();
	lod_op_overflow_ = 0;
}

} // namespace godot
