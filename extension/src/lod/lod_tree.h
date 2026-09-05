#pragma once
#include "lod/lod_grid.h"
#include "world/region.h"
#include <cstdint>
#include <map>
#include <vector>

namespace ve {

// Eviction age: an unmarked non-resident node is freed after this many frames.
inline constexpr uint32_t kLodEvictFrames = 300;
// Occlusion is only trusted after this many consecutive frames agree (stale readback safety).
inline constexpr uint32_t kLodOccludedFrames = 8;

// Everything the walk needs to know about the view. view_proj is COLUMN-MAJOR in GLSL order
// (index = column * 4 + row), which is what godot::Projection hands over and what the raster
// pass pushes, so there is exactly one convention.
struct LodCamera {
	float view_proj[16] = {};
	float pos[3] = {};
	int viewport[2] = {1, 1};
};

// A REVERSE-Z perspective camera (near -> 1, far -> 0), matching Godot 4.7.1's depth-corrected
// scene projection (M1 errata 2). Production passes Godot's own matrix; this exists so tests
// can state a camera in one line and still exercise the same depth convention.
LodCamera lod_camera_perspective(const float pos[3], const float fwd[3], const float up[3],
		float fov_y_rad, float aspect, float z_near, float z_far, int vw, int vh);

// How the walk asks whether something is hidden. An interface for the same reason
// ve::ChunkProbe is one: the real answer lives in a GPU readback on the Godot side of the
// wall, and a test must be able to answer it in two lines.
struct LodOcclusion {
	virtual ~LodOcclusion() = default;
	// Screen-space AABB: xy in [0, 1], z the REVERSE-Z depth (larger = nearer). Returns true
	// when everything in that box is behind an already-drawn surface.
	virtual bool occluded(const float ss_min[3], const float ss_max[3]) const = 0;
};

enum LodNodeState : uint8_t {
	kLodUnknown = 0,
	kLodBuilding = 1,
	kLodReady = 2,
	kLodEmpty = 3,
	kLodFailed = 4,
};

struct LodDrawItem {
	int level = 0;
	IVec3 coord{};
	int page_first = -1;
	int page_count = 0;
};

// A chunk key used by the page map. IVec3 has no operator<, so the map key is explicit.
struct LodKey {
	int level = 0;
	int x = 0;
	int y = 0;
	int z = 0;
	bool operator<(const LodKey &o) const {
		if (level != o.level) return level < o.level;
		if (z != o.z) return z < o.z;
		if (y != o.y) return y < o.y;
		return x < o.x;
	}
};

// One indirect draw command for one arena page. The raster pass consumes these.
struct LodPageDraw {
	int page = -1;
	int quad_count = 0;
};

// Emits one LodPageDraw per actual page in `pages_of` for each draw item. The tree's
// page_first/page_count fields are kept for eviction/tests but are NOT used here: LodArena can
// hand a chunk non-contiguous pages, and only the real page list says what to draw.
void lod_collect_page_draws(const std::vector<LodDrawItem> &draws,
		const std::map<LodKey, std::vector<int>> &pages_of,
		const std::map<int, int> &page_quads, std::vector<LodPageDraw> *out);

struct LodBuildRequest {
	int level = 0;
	IVec3 coord{};
	float priority = 0.0f; // projected screen area in px^2; larger is built first
};

struct LodWalkResult {
	std::vector<LodDrawItem> draws;
	// The same cut as `draws`, minus the frustum test: what the SUN has to rasterize.
	//
	// A shadow map is not rendered from the camera, so terrain beside and behind it must
	// keep casting -- but it still has to be a CUT, one description of each piece of ground.
	// Feeding the map every RESIDENT page instead puts several LoD descriptions of the same
	// ground in one texel; they disagree by metres (levels 5-7 are 12.8-51.2 m cells and are
	// never evicted, so their tent-filtered surfaces bulge over the whole world), and
	// whichever one happens to survive the depth test then shadows open sunlit ground.
	std::vector<LodDrawItem> shadow_draws;
	std::vector<LodBuildRequest> requests;
};

struct LodTreeConfig {
	// The far field's extent. There is no world edge: the octree's roots are the level-7
	// chunks intersecting a sphere of this radius around the camera, and nodes beyond it lose
	// their eviction exemption (see collect_evictions).
	float stream_radius_m = 1638.4f;
	float sse_area_thresh = kLodSseAreaThresh;
	int resident_level_from = kLodResidentLevelFrom;
	uint32_t evict_frames = kLodEvictFrames;
	uint32_t occluded_frames = kLodOccludedFrames;
	int max_requests_per_walk = 32;
	float fade_start_m = kLodFadeStartM;
};

// Spec section 6. Residency is what the walk touched, not what is near.
class LodTree {
public:
	explicit LodTree(const LodTreeConfig &cfg);

	// One walk per frame against the CURRENT camera. `occ` may be null (no readback yet).
	void walk(const LodCamera &cam, const LodOcclusion *occ, uint32_t frame, LodWalkResult *out);

	void note_building(int level, IVec3 c);
	void note_ready(int level, IVec3 c, int page_first, int page_count);
	// Stale pages stay drawable while a rebuild is refused and must be retried:
	// this is like note_ready but keeps the node's existing page range AND leaves
	// dirty=true, so the next walk still re-requests it.
	void note_ready_dirty(int level, IVec3 c);
	void note_empty(int level, IVec3 c);
	void note_failed(int level, IVec3 c);

	// Every level whose chunks the world AABB touches is re-requested. A drawn node keeps
	// drawing its stale pages until the rebuild lands -- stale beats missing.
	void mark_dirty(const float lo[3], const float hi[3]);

	// Nodes to free. Age-based when want_pages == 0; under arena pressure it additionally
	// evicts least-recently-marked first until want_pages have been recovered. Levels at or
	// above resident_level_from are exempt.
	void collect_evictions(uint32_t frame, int want_pages, std::vector<LodDrawItem> *out);

	int state_of(int level, IVec3 c) const;
	// Whether the node has a pending edit that still needs a rebuild. A stale empty result
	// can be distinguished from a genuinely empty chunk by checking this before treating
	// kLodEmpty as terminal.
	bool is_dirty(int level, IVec3 c) const;
	// Reports how many nodes are dirty and how many distinct levels they occupy.
	void dirty_stats(int *chunks, int *levels) const;
	int node_count() const { return static_cast<int>(nodes_.size()); }
	void clear();
	const LodTreeConfig &config() const { return cfg_; }
	// The seam follows the near field's measured reach (ve::lod_fade_band), so the build
	// gate has to move with it: chunks the fragment shader would now keep must be built.
	void set_fade_start_m(float v) { cfg_.fade_start_m = v; }

private:
	struct Key {
		int level, x, y, z;
		bool operator<(const Key &o) const {
			if (level != o.level) return level < o.level;
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	struct Node {
		uint8_t state = kLodUnknown;
		bool building = false; // a build is in flight; ready nodes keep drawing old pages
		bool dirty = false;
		int page_first = -1;
		int page_count = 0;
		uint32_t last_marked = 0;
		uint32_t occluded_since = 0; // 0 = not currently occluded
	};
	static Key key(int level, IVec3 c) { return Key{level, c.x, c.y, c.z}; }

	void visit(int level, IVec3 c, const LodCamera &cam, const LodOcclusion *occ,
			uint32_t frame, LodWalkResult *out);
	// The frustum-free twin of visit(): same descend rule, no state touched, no requests.
	void shadow_visit(int level, IVec3 c, const LodCamera &cam,
			std::vector<LodDrawItem> *out) const;
	// Whether the cut descends below this node. ONE rule, read by the camera walk and by the
	// shadow cut, so the two can never choose different levels for the same ground -- which
	// is exactly what puts two surfaces metres apart in one shadow texel.
	bool want_finer(int level, IVec3 c, float area) const;
	bool children_ready(int level, IVec3 c) const;
	void request(int level, IVec3 c, float area, LodWalkResult *out,
			bool touch_residency = true);

	LodTreeConfig cfg_;
	std::map<Key, Node> nodes_;
	float planes_[6][4] = {};  // scratch, rebuilt per walk
	float last_cam_pos_[3] = {}; // scratch, rebuilt per walk; read by request()
	uint32_t last_walk_frame_ = 0; // current walk frame; marks request/ready residency
};

// Exposed for testing: the six frustum planes of a view-projection, and Voxy's exact
// projected-area measure (screenspace.glsl's shouldDecend), which is the silhouette area
// rather than the screen AABB -- an AABB over-estimates a diagonal box by 2-3x and would
// over-tessellate everywhere.
void lod_frustum_planes(const float view_proj[16], float out[6][4]);
bool lod_aabb_in_frustum(const float planes[6][4], const float lo[3], const float hi[3]);
// Returns the area in px^2, or a very large value when the box straddles the near plane
// (where the perspective divide is meaningless and the only safe answer is "descend").
float lod_projected_area(const LodCamera &cam, const float lo[3], const float hi[3],
		float ss_min[3], float ss_max[3]);

} // namespace ve
