#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "shade/sun_cascades.h"
#include "shade/sun_ortho.h"

namespace godot {

class LodPool;
class LodRasterPass;

// Three nested camera-centred shadow maps in one array texture. One pass rather than three,
// so the fit stays written down in one place -- the property VoxelWorld::sun_ortho() exists
// to hold: the debug facade reads the same function the render path does, so what the tests
// pin is what ships.
//
// Per-layer dirty state is what makes the far cascade affordable. Each cascade snaps its
// light-space origin to its OWN texel (ve::sun_ortho_sphere), so cascade 0 re-projects about
// every 0.4 m of camera travel and cascade 2 about every 3.9 m: the expensive one rebuilds
// rarely and the cheap one rebuilds often, with no new throttle invented.
class SunShadowPass {
public:
	static constexpr int kSize = 2048;
	static constexpr int kMinFrames = 12;
	static constexpr int kCascades = ve::kSunCascades;

	~SunShadowPass();
	bool initialize(RenderingDevice *rd);
	void teardown();
	// Dirties every cascade: a page appearing or leaving can change any of them.
	void mark_dirty();

	// Whether build() would do work for this cascade. Split out of build() so a caller can
	// skip producing the cascade's LoD cut too -- for cascade 2 that cut is the expensive
	// half, and it is skipped on most frames. THE POLL ADVANCES THE THROTTLE: every call
	// moves frames_since one step, then evaluates the shared rule, so the first build
	// fires kMinFrames polls after the cascade dirties. The advance must live here and
	// not in build(), because the compositor skips build() when the poll declines -- a
	// counter advanced only inside build() could never reach kMinFrames and no cascade
	// would ever build in the game path. MUST stay in agreement with build()'s own
	// early-out; both call the same private test rather than stating the rule twice.
	bool needs_rebuild(int cascade, const ve::SunOrtho &ortho);

	// The same rule as a pure snapshot: no counter advance. For display and diagnostics
	// (debug_sun_shadow_stats' rebuild_pending pin), where reading must not perturb the
	// throttle the poll above drives.
	bool rebuild_pending(int cascade, const ve::SunOrtho &ortho) const;

	bool build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster, int cascade,
			const ve::SunOrtho &ortho, bool force);

	RID map() const { return map_; } // 2048 x 2048 x kCascades, D32_SFLOAT
	int cascade_count() const { return kCascades; }
	const float *view_proj(int cascade) const { return c_[clamp_index(cascade)].view_proj; }
	float texel_world(int cascade) const { return c_[clamp_index(cascade)].texel_world; }
	float depth_range(int cascade) const { return c_[clamp_index(cascade)].depth_range; }
	int rebuilds(int cascade) const { return c_[clamp_index(cascade)].rebuilds; }
	int last_pages(int cascade) const { return c_[clamp_index(cascade)].last_pages; }
	bool is_valid() const { return rd_ && map_.is_valid() && shader_.is_valid(); }

private:
	struct Cascade {
		RID slice;       // texture_create_shared_from_slice, one array layer
		RID framebuffer; // that slice as a depth attachment
		bool dirty = true;
		int frames_since = 0;
		int rebuilds = 0;
		int last_pages = 0;
		float view_proj[16] = {};
		float texel_world = 0.0f;
		float depth_range = 0.0f;
	};

	static int clamp_index(int cascade) {
		return cascade < 0 ? 0 : (cascade >= kCascades ? kCascades - 1 : cascade);
	}
	// THE rebuild rule, stated once. needs_rebuild() and build() both call it.
	bool should_rebuild(const Cascade &c, const ve::SunOrtho &ortho, bool force) const;
	bool ensure_pipeline(RenderingDevice *rd);
	bool ensure_uniform_set(RenderingDevice *rd, LodPool &pool);

	RenderingDevice *rd_ = nullptr;
	RID map_;
	RID shader_;
	RID pipeline_;
	RID uset_;
	RID key_quads_;
	RID key_page_chunk_;
	RID key_chunks_;
	Cascade c_[kCascades];
};

} // namespace godot
