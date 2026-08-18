#include "lod/lod_tree.h"
#include <algorithm>
#include <cmath>

namespace ve {

namespace {

void mat_mul_vec(const float m[16], const float v[4], float out[4]) {
	for (int r = 0; r < 4; r++)
		out[r] = m[0 * 4 + r] * v[0] + m[1 * 4 + r] * v[1] + m[2 * 4 + r] * v[2] +
				m[3 * 4 + r] * v[3];
}

float cross_mag(const float a[2], const float b[2]) {
	return std::fabs(a[0] * b[1] - b[0] * a[1]);
}

void normalize3(float v[3]) {
	const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
	if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

} // namespace

void lod_collect_page_draws(const std::vector<LodDrawItem> &draws,
		const std::map<LodKey, std::vector<int>> &pages_of,
		const std::map<int, int> &page_quads, std::vector<LodPageDraw> *out) {
	if (!out) return;
	out->clear();
	for (const LodDrawItem &item : draws) {
		const LodKey key{item.level, item.coord.x, item.coord.y, item.coord.z};
		const auto pages_it = pages_of.find(key);
		if (pages_it == pages_of.end()) continue;
		for (int p : pages_it->second) {
			const auto quads_it = page_quads.find(p);
			if (quads_it == page_quads.end()) continue;
			out->push_back(LodPageDraw{p, quads_it->second});
		}
	}
}

LodCamera lod_camera_perspective(const float pos[3], const float fwd[3], const float up[3],
		float fov_y_rad, float aspect, float z_near, float z_far, int vw, int vh) {
	LodCamera c;
	c.pos[0] = pos[0]; c.pos[1] = pos[1]; c.pos[2] = pos[2];
	c.viewport[0] = vw;
	c.viewport[1] = vh;

	float f[3] = {fwd[0], fwd[1], fwd[2]};
	normalize3(f);
	float s[3] = {f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2],
			f[0] * up[1] - f[1] * up[0]};
	normalize3(s);
	const float u[3] = {s[1] * f[2] - s[2] * f[1], s[2] * f[0] - s[0] * f[2],
			s[0] * f[1] - s[1] * f[0]};

	// View matrix (column-major).
	float v[16] = {};
	v[0] = s[0]; v[4] = s[1]; v[8] = s[2];
	v[1] = u[0]; v[5] = u[1]; v[9] = u[2];
	v[2] = -f[0]; v[6] = -f[1]; v[10] = -f[2];
	v[12] = -(s[0] * pos[0] + s[1] * pos[1] + s[2] * pos[2]);
	v[13] = -(u[0] * pos[0] + u[1] * pos[1] + u[2] * pos[2]);
	v[14] = f[0] * pos[0] + f[1] * pos[1] + f[2] * pos[2];
	v[15] = 1.0f;

	// REVERSE-Z perspective: z maps near -> 1, far -> 0 (M1 errata 2).
	const float t = 1.0f / std::tan(fov_y_rad * 0.5f);
	float p[16] = {};
	p[0] = t / aspect;
	p[5] = t;
	p[10] = z_near / (z_far - z_near);
	p[11] = -1.0f;
	p[14] = (z_far * z_near) / (z_far - z_near);

	for (int col = 0; col < 4; col++)
		for (int row = 0; row < 4; row++) {
			float acc = 0.0f;
			for (int k = 0; k < 4; k++) acc += p[k * 4 + row] * v[col * 4 + k];
			c.view_proj[col * 4 + row] = acc;
		}
	return c;
}

void lod_frustum_planes(const float m[16], float out[6][4]) {
	// Gribb-Hartmann on a column-major matrix: row r of the matrix is m[c*4 + r].
	const auto row = [&](int r, int c) { return m[c * 4 + r]; };
	for (int i = 0; i < 6; i++) {
		const int r = i >> 1;
		const float sgn = (i & 1) ? -1.0f : 1.0f;
		for (int c = 0; c < 4; c++) out[i][c] = row(3, c) + sgn * row(r, c);
	}
	for (int i = 0; i < 6; i++) {
		const float l = std::sqrt(out[i][0] * out[i][0] + out[i][1] * out[i][1] +
				out[i][2] * out[i][2]);
		if (l > 0.0f)
			for (int c = 0; c < 4; c++) out[i][c] /= l;
	}
}

bool lod_aabb_in_frustum(const float planes[6][4], const float lo[3], const float hi[3]) {
	for (int i = 0; i < 6; i++) {
		// The AABB corner farthest along the plane normal. If even that is behind the plane,
		// the whole box is.
		const float px = planes[i][0] >= 0.0f ? hi[0] : lo[0];
		const float py = planes[i][1] >= 0.0f ? hi[1] : lo[1];
		const float pz = planes[i][2] >= 0.0f ? hi[2] : lo[2];
		if (planes[i][0] * px + planes[i][1] * py + planes[i][2] * pz + planes[i][3] < 0.0f)
			return false;
	}
	return true;
}

float lod_projected_area(const LodCamera &cam, const float lo[3], const float hi[3],
		float ss_min[3], float ss_max[3]) {
	float ss[8][3];
	for (int k = 0; k < 8; k++) {
		const float p[4] = {(k & 1) ? hi[0] : lo[0], (k & 2) ? hi[1] : lo[1],
				(k & 4) ? hi[2] : lo[2], 1.0f};
		float clip[4];
		mat_mul_vec(cam.view_proj, p, clip);
		// Straddling the near plane makes the perspective divide meaningless. The only safe
		// answer is "this is enormous, descend" -- and no occlusion claim can be made.
		if (clip[3] <= 1e-4f) {
			ss_min[0] = ss_min[1] = ss_min[2] = 0.0f;
			ss_max[0] = ss_max[1] = ss_max[2] = 1.0f;
			return 3.4e38f;
		}
		const float inv = 1.0f / clip[3];
		ss[k][0] = (clip[0] * inv * 0.5f + 0.5f) * float(cam.viewport[0]);
		ss[k][1] = (clip[1] * inv * 0.5f + 0.5f) * float(cam.viewport[1]);
		ss[k][2] = clip[2] * inv;
	}
	for (int a = 0; a < 3; a++) {
		ss_min[a] = ss[0][a];
		ss_max[a] = ss[0][a];
		for (int k = 1; k < 8; k++) {
			ss_min[a] = std::min(ss_min[a], ss[k][a]);
			ss_max[a] = std::max(ss_max[a], ss[k][a]);
		}
	}
	// Voxy's exact silhouette measure: the three faces meeting at corner 000 plus the three
	// meeting at 111, halved because that counts front and back.
	const auto edge = [&](int from, int to, float d[2]) {
		d[0] = ss[to][0] - ss[from][0];
		d[1] = ss[to][1] - ss[from][1];
	};
	float A[2], B[2], C[2];
	float area = 0.0f;
	edge(0, 1, A); edge(0, 2, B); edge(0, 4, C);
	area += cross_mag(A, B) + cross_mag(A, C) + cross_mag(C, B);
	edge(7, 6, A); edge(7, 5, B); edge(7, 3, C);
	area += cross_mag(A, B) + cross_mag(A, C) + cross_mag(C, B);
	area *= 0.5f;

	// Normalise the screen box to [0, 1] for the occlusion interface.
	ss_min[0] /= float(cam.viewport[0]); ss_max[0] /= float(cam.viewport[0]);
	ss_min[1] /= float(cam.viewport[1]); ss_max[1] /= float(cam.viewport[1]);
	for (int a = 0; a < 2; a++) {
		ss_min[a] = std::max(0.0f, std::min(ss_min[a], 1.0f));
		ss_max[a] = std::max(0.0f, std::min(ss_max[a], 1.0f));
	}
	return area;
}

LodTree::LodTree(const LodTreeConfig &cfg) : cfg_(cfg) {}

void LodTree::clear() { nodes_.clear(); }

int LodTree::state_of(int level, IVec3 c) const {
	const auto it = nodes_.find(key(level, c));
	return it == nodes_.end() ? -1 : int(it->second.state);
}

bool LodTree::is_dirty(int level, IVec3 c) const {
	const auto it = nodes_.find(key(level, c));
	return it != nodes_.end() && it->second.dirty;
}

void LodTree::note_building(int level, IVec3 c) {
	Node &n = nodes_[key(level, c)];
	n.building = true;
	// The in-flight build is for the state as of submission; clear any dirty that the walk
	// observed. Edits that land AFTER this call re-set dirty and are preserved by note_ready.
	n.dirty = false;
	// A ready node stays ready so it keeps drawing its old pages (stale beats missing). A
	// non-ready node stays non-drawable; kLodBuilding remains a convenient marker for that.
	if (n.state != kLodReady) n.state = kLodBuilding;
}

void LodTree::note_ready(int level, IVec3 c, int page_first, int page_count) {
	Node &n = nodes_[key(level, c)];
	const bool was_building = n.building;
	n.building = false;
	n.state = kLodReady;
	// Clear dirty only when this was not an in-flight build. note_building already cleared it
	// at submission; if an edit re-set it while the build was in flight, note_ready must keep
	// it so the next walk rebuilds again. Direct note_ready callers (tests/settle helpers)
	// still get the old unconditional-clear behaviour.
	if (!was_building) n.dirty = false;
	n.page_first = page_first;
	n.page_count = page_count;
	// A freshly ready child may still be waiting for its siblings before it is visited;
	// mark it now so the sibling gate cannot let age eviction recycle it first.
	n.last_marked = last_walk_frame_;
}

void LodTree::note_ready_dirty(int level, IVec3 c) {
	// Stale pages stay drawable while a rebuild is refused and must be retried. Unlike
	// note_ready, this transition keeps dirty=true (so the next walk re-requests the node)
	// and leaves the existing page_first/page_count untouched (so the old pages keep
	// drawing until the retry succeeds). Mark it as resident so eviction does not reclaim
	// the old pages while the retry is pending.
	Node &n = nodes_[key(level, c)];
	n.building = false;
	n.state = kLodReady;
	n.dirty = true;
	n.last_marked = last_walk_frame_;
}

void LodTree::note_empty(int level, IVec3 c) {
	Node &n = nodes_[key(level, c)];
	const bool was_building = n.building;
	n.building = false;
	if (was_building && n.dirty) {
		// A stale empty result from an in-flight build must not hide an edit that landed
		// after note_building. Leave the node requestable and keep dirty so the next walk
		// rebuilds with the edit included.
		n.state = kLodUnknown;
		n.page_first = -1;
		n.page_count = 0;
		return;
	}
	n.state = kLodEmpty;
	n.dirty = false;
	n.page_first = -1;
	n.page_count = 0;
}

void LodTree::note_failed(int level, IVec3 c) {
	Node &n = nodes_[key(level, c)];
	n.building = false;
	n.state = kLodFailed;
}

bool LodTree::children_ready(int level, IVec3 c) const {
	if (level <= 0) return false;
	const IVec3 base = lod_child_base(c);
	for (int k = 0; k < 8; k++) {
		const IVec3 ch{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
		if (!lod_chunk_in_bounds(cfg_.bounds, level - 1, ch)) continue; // outside is "done"
		const auto it = nodes_.find(key(level - 1, ch));
		if (it == nodes_.end()) return false;
		if (it->second.state != kLodReady && it->second.state != kLodEmpty) return false;
	}
	return true;
}

void LodTree::request(int level, IVec3 c, float area, LodWalkResult *out,
		bool touch_residency) {
	if (!lod_chunk_in_bounds(cfg_.bounds, level, c)) return;
	// Never build what the fragment shader would discard on every pixel (spec section 6.4).
	if (lod_chunk_far_distance(level, c, last_cam_pos_) < cfg_.fade_start_m) return;
	Node &n = nodes_[key(level, c)];
	if (n.building) return;
	if (n.state == kLodBuilding) return;
	if (n.state == kLodEmpty) return;
	if (n.state == kLodReady && !n.dirty) return;
	// A request is itself a residency touch: speculative child requests are not visited
	// until all siblings are ready, so without this mark they could be evicted first.
	// The dirty sweep must opt out: it re-requests off-screen stale nodes purely to rebuild
	// them, and touching residency there would keep those stale pages resident forever.
	if (touch_residency) n.last_marked = last_walk_frame_;
	out->requests.push_back(LodBuildRequest{level, c, area});
}

void LodTree::visit(int level, IVec3 c, const LodCamera &cam, const LodOcclusion *occ,
		uint32_t frame, LodWalkResult *out) {
	if (!lod_chunk_in_bounds(cfg_.bounds, level, c)) return;
	float lo[3], hi[3];
	lod_chunk_aabb(level, c, lo, hi);

	Node &n = nodes_[key(level, c)];
	n.last_marked = frame; // touched, therefore resident: this is the whole eviction rule

	if (!lod_aabb_in_frustum(planes_, lo, hi)) return;

	float ss_min[3], ss_max[3];
	const float area = lod_projected_area(cam, lo, hi, ss_min, ss_max);

	if (occ && area < 3.0e38f && occ->occluded(ss_min, ss_max)) {
		if (n.occluded_since == 0) n.occluded_since = frame;
	} else {
		n.occluded_since = 0;
	}
	const bool refine_blocked = n.occluded_since != 0 &&
			(frame - n.occluded_since) >= cfg_.occluded_frames;

	if (n.state != kLodReady) {
		// Not drawable. Ask for it (unless occlusion says nobody would see it) and stop:
		// there is nothing below a node we do not have.
		if (n.state != kLodEmpty && !refine_blocked) request(level, c, area, out);
		return;
	}

	const bool want_finer = level > 0 && area > cfg_.sse_area_thresh;
	if (want_finer && children_ready(level, c)) {
		const IVec3 base = lod_child_base(c);
		for (int k = 0; k < 8; k++)
			visit(level - 1, {base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)},
					cam, occ, frame, out);
		return;
	}

	out->draws.push_back(LodDrawItem{level, c, n.page_first, n.page_count});
	if (n.dirty && !refine_blocked) request(level, c, area, out);
	if (want_finer && !refine_blocked) {
		const IVec3 base = lod_child_base(c);
		for (int k = 0; k < 8; k++) {
			const IVec3 ch{base.x + (k & 1), base.y + ((k >> 1) & 1), base.z + ((k >> 2) & 1)};
			// The child inherits its parent's area as its priority: the parent is what the
			// viewer is actually looking at, and eight children of one parent should arrive
			// together or the sibling gate never opens.
			request(level - 1, ch, area, out);
		}
	}
}

void LodTree::walk(const LodCamera &cam, const LodOcclusion *occ, uint32_t frame,
		LodWalkResult *out) {
	out->draws.clear();
	out->requests.clear();
	last_walk_frame_ = frame;
	lod_frustum_planes(cam.view_proj, planes_);
	last_cam_pos_[0] = cam.pos[0];
	last_cam_pos_[1] = cam.pos[1];
	last_cam_pos_[2] = cam.pos[2];

	IVec3 lo{}, hi{};
	lod_root_range(cfg_.bounds, &lo, &hi);
	for (int z = lo.z; z <= hi.z; z++)
		for (int y = lo.y; y <= hi.y; y++)
			for (int x = lo.x; x <= hi.x; x++)
				visit(kLodLevels - 1, {x, y, z}, cam, occ, frame, out);

	// Edits mark nodes at every level they touch, including levels the current cut does not
	// visit. Consider every dirty node for re-request so a rebuild is not deferred until the
	// camera happens to refine that deep -- the edit must reach the far field now. This is
	// gathered BEFORE sorting/dedup so dirty requests participate in the same priority order
	// and duplicate entries are removed before the per-walk cap can starve the tail.
	for (const auto &kv : nodes_) {
		if (!kv.second.dirty) continue;
		const IVec3 c{kv.first.x, kv.first.y, kv.first.z};
		request(kv.first.level, c, 0.0f, out, false);
	}

	std::sort(out->requests.begin(), out->requests.end(),
			[](const LodBuildRequest &a, const LodBuildRequest &b) {
				if (a.priority != b.priority) return a.priority > b.priority;
				if (a.level != b.level) return a.level > b.level; // coarse first: the gate
				if (a.coord.z != b.coord.z) return a.coord.z < b.coord.z;
				if (a.coord.y != b.coord.y) return a.coord.y < b.coord.y;
				return a.coord.x < b.coord.x;
			});

	// De-duplicate by (level, coord) preserving the highest-priority entry (the first after
	// the sort above). std::unique is not enough: a dirty sweep duplicate can be separated
	// from its normal-walk copy by requests of other priorities.
	std::map<Key, bool> seen;
	auto last = std::remove_if(out->requests.begin(), out->requests.end(),
			[&](const LodBuildRequest &q) {
				return !seen.emplace(Key{q.level, q.coord.x, q.coord.y, q.coord.z}, true).second;
			});
	out->requests.erase(last, out->requests.end());
	if (int(out->requests.size()) > cfg_.max_requests_per_walk)
		out->requests.resize(size_t(cfg_.max_requests_per_walk));
}

void LodTree::mark_dirty(const float lo[3], const float hi[3]) {
	EditOp probe;
	probe.type = kOpSphereSubtract;
	for (int a = 0; a < 3; a++) probe.pos[a] = 0.5f * (lo[a] + hi[a]);
	probe.radius = 0.5f * std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
	const float longest = std::max(std::max(hi[0] - lo[0], hi[1] - lo[1]), hi[2] - lo[2]);
	for (int level = 0; level < kLodLevels; level++) {
		// The reduced lattice samples every half cell, so an edit shorter than half a cell
		// on every axis cannot move a sample at this level and needs no rebuild.
		if (longest < 0.5f * lod_cell_size(level)) continue;
		IVec3 clo{}, chi{};
		op_lod_chunk_range(probe, level, &clo, &chi);
		for (int z = clo.z; z <= chi.z; z++)
			for (int y = clo.y; y <= chi.y; y++)
				for (int x = clo.x; x <= chi.x; x++) {
					const auto it = nodes_.find(key(level, {x, y, z}));
					if (it == nodes_.end()) continue;
					// A cached "empty" would hide a surface an add-op just put there.
					if (it->second.state == kLodEmpty) it->second.state = kLodUnknown;
					it->second.dirty = true;
				}
	}
}

void LodTree::dirty_stats(int *chunks, int *levels) const {
	if (!chunks || !levels) return;
	*chunks = 0;
	*levels = 0;
	bool seen[kLodLevels] = {};
	for (const auto &kv : nodes_) {
		if (!kv.second.dirty) continue;
		(*chunks)++;
		if (!seen[kv.first.level]) {
			seen[kv.first.level] = true;
			(*levels)++;
		}
	}
}

void LodTree::collect_evictions(uint32_t frame, int want_pages, std::vector<LodDrawItem> *out) {
	out->clear();
	struct Cand {
		Key k;
		uint32_t age;
		int pages;
		int page_first;
	};
	std::vector<Cand> cands;
	for (const auto &kv : nodes_) {
		if (kv.first.level >= cfg_.resident_level_from) continue;
		if (kv.second.building) continue;
		if (kv.second.state == kLodBuilding) continue;
		const uint32_t age = frame >= kv.second.last_marked ? frame - kv.second.last_marked : 0u;
		cands.push_back(Cand{kv.first, age, kv.second.page_count, kv.second.page_first});
	}
	std::sort(cands.begin(), cands.end(),
			[](const Cand &a, const Cand &b) { return a.age > b.age; });

	int recovered = 0;
	for (const Cand &c : cands) {
		const bool too_old = c.age > cfg_.evict_frames;
		const bool pressure = want_pages > 0 && recovered < want_pages && c.age > 0;
		if (!too_old && !pressure) continue;
		out->push_back(LodDrawItem{c.k.level, IVec3{c.k.x, c.k.y, c.k.z}, c.page_first, c.pages});
		recovered += c.pages;
	}
	for (const LodDrawItem &d : *out) nodes_.erase(key(d.level, d.coord));
}

} // namespace ve
