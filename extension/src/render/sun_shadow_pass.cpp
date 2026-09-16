#include "render/sun_shadow_pass.h"
#include "render/lod_pool.h"
#include "render/lod_raster_pass.h"
#include "gpu_layout/blocks.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

SunShadowPass::~SunShadowPass() {
	teardown();
}

bool SunShadowPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	Ref<RDTextureFormat> tf;
	tf.instantiate();
	tf->set_format(RenderingDevice::DATA_FORMAT_D32_SFLOAT);
	tf->set_width(kSize);
	tf->set_height(kSize);
	tf->set_array_layers(kCascades);
	tf->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	tf->set_usage_bits(RenderingDevice::TEXTURE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
			RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT);
	Ref<RDTextureView> tv;
	tv.instantiate();
	map_ = group_.add(gpu::Kind::Texture, rd_->texture_create(tf, tv, {}));
	if (!map_.is_valid()) {
		UtilityFunctions::printerr("SunShadowPass: shadow map array creation failed");
		teardown();
		return false;
	}

	// One framebuffer per layer. A shared slice is what lets cascade 0's frequent rebuilds
	// be isolated from cascade 2's expensive one; a single atlased framebuffer would force
	// them to rebuild together and throw the amortization away.
	for (int i = 0; i < kCascades; i++) {
		c_[i].slice = group_.add(gpu::Kind::Texture,
				rd_->texture_create_shared_from_slice(tv, map_, i, 0));
		if (!c_[i].slice.is_valid()) {
			UtilityFunctions::printerr("SunShadowPass: cascade slice ", i, " failed");
			teardown();
			return false;
		}
	}

	shader_ = gpu::compile_raster(rd_, group_, "SunShadowPass", "lod_shadow.vert.glsl",
			"lod_shadow.frag.glsl");
	if (!shader_.is_valid()) {
		teardown();
		return false;
	}
	return true;
}

void SunShadowPass::teardown() {
	if (rd_) {
		gpu::RdDevice device{rd_};
		group_.release(device);
	}
	for (int i = 0; i < kCascades; i++) c_[i] = Cascade{};
	map_ = shader_ = pipeline_ = RID();
	set_ = gpu::SetCache();
	rd_ = nullptr;
}

void SunShadowPass::mark_dirty() {
	// A page appearing or leaving can change any cascade, so all of them are dirtied. The
	// per-cascade throttle is what keeps that from costing three full rasters.
	for (int i = 0; i < kCascades; i++) {
		c_[i].dirty = true;
		c_[i].frames_since = 0;
	}
}

bool SunShadowPass::ensure_pipeline(RenderingDevice *rd) {
	if (rd->framebuffer_is_valid(c_[0].framebuffer) && pipeline_.is_valid()) return true;
	gpu::RdDevice device{rd};
	for (int i = 0; i < kCascades; i++) {
		group_.free(device, c_[i].framebuffer);
		c_[i].framebuffer = group_.add(gpu::Kind::Framebuffer,
				rd->framebuffer_create(Array::make(c_[i].slice)));
		if (!rd->framebuffer_is_valid(c_[i].framebuffer)) return false;
	}
	const int64_t format = rd->framebuffer_get_format(c_[0].framebuffer);
	group_.free(device, pipeline_);
	gpu::RasterState state;
	// NO CULLING, and no borrowed front-face convention.
	//
	// Winding is a property of the PROJECTION, not of the geometry: this pass inherited
	// LodRasterPass::front_face_clockwise(), which was measured against the camera's
	// reverse-Z perspective, and the sun's ortho has the opposite handedness. Front and back
	// therefore swapped here, and the pass culled precisely the up-facing terrain quads it
	// exists to record. What survived was the skirt curtains, which lod_append_skirts emits
	// twice with opposite winding -- so the map held a wireframe of chunk boundaries and
	// almost none of the ground, which is why its shadows had the wrong shape.
	//
	// Culling is only ever an optimisation for a depth-only pass. Letting GREATER_OR_EQUAL
	// keep whichever surface is nearest the sun is what a shadow map means, it is correct
	// under any projection, and it cannot be silently inverted by a matrix change again.
	// The cost is bounded: both faces instead of one, but over the cut rather than the whole
	// resident set (2313 -> 1687 pages on the demo view), on a depth-only target that
	// rebuilds at most once every kMinFrames.
	state.cull = RenderingDevice::POLYGON_CULL_DISABLED;
	// Reverse-Z (near the sun = 1): GREATER_OR_EQUAL keeps the surface NEAREST the sun, which
	// is what a shadow map means. An overhang's roof therefore wins over its own floor, and
	// two chunks that overlap under a low sun resolve by geometry rather than by draw order.
	//
	// It was briefly ALWAYS, to let a coarsest-first page list end with the finest description
	// of each texel. That was a workaround for the page list carrying several LoD levels of
	// the same ground at once; LodSystem::prepare_shadow_raster now sends a cut instead, so
	// there is one surface per texel and the honest test is back.
	state.compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	state.color_attachments = 0;
	pipeline_ = gpu::raster_pipeline(rd, group_, shader_, format, state);
	return pipeline_.is_valid();
}

bool SunShadowPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool) {
	gpu::RdDevice device{rd};
	return set_.get(device, group_, shader_, 0, {
			gpu::storage(0, pool.quad_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer())}).is_valid();
}

// THE rebuild rule, in one place. The projection moving means what is stored no longer
// describes what will be sampled: rebuild now rather than at the throttle's convenience.
// kMinFrames exists to damp pages coming and going; it must not make a day/night sweep lag
// twelve frames behind the sun, nor leave a cascade a texel behind the camera it follows.
//
// Comparing the whole matrix is affordable precisely because sun_ortho_sphere() snaps: an
// unsnapped camera-following fit differs on every frame the camera moves at all, and this
// test would degenerate into a full 2048^2 pass over the whole cut, every frame.
bool SunShadowPass::should_rebuild(const Cascade &c, const ve::SunOrtho &ortho,
		bool force) const {
	if (!is_valid() || !ortho.valid) return false;
	const bool projection_moved = c.rebuilds > 0 &&
			std::memcmp(c.view_proj, ortho.view_proj, sizeof(c.view_proj)) != 0;
	if (force || projection_moved) return true;
	return c.dirty && c.frames_since >= kMinFrames;
}

bool SunShadowPass::needs_rebuild(int cascade, const ve::SunOrtho &ortho) {
	// The throttle advance lives HERE, in the per-frame poll, not in build(): the
	// compositor asks this once per cascade per frame and skips build() when it
	// declines, so a counter advanced only inside build() would sit at 0 forever and
	// the gate could never open. build() keeps the same rule as a pure re-check.
	Cascade &c = c_[clamp_index(cascade)];
	c.frames_since++;
	return should_rebuild(c, ortho, false);
}

bool SunShadowPass::rebuild_pending(int cascade, const ve::SunOrtho &ortho) const {
	return should_rebuild(c_[clamp_index(cascade)], ortho, false);
}

bool SunShadowPass::build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster,
		int cascade, const ve::SunOrtho &ortho, bool force) {
	Cascade &c = c_[clamp_index(cascade)];
	// Pure gate: the throttle advance happens in needs_rebuild(), which the caller asks
	// first (the compositor polls, then skips this call when the poll declines).
	if (!should_rebuild(c, ortho, force)) return false;
	const std::vector<LodRasterPass::PageDraw> &pages = raster.draw_pages();
	if (pages.empty()) return false;
	if (!raster.prepare_index_array(rd, pool)) return false;
	if (!ensure_pipeline(rd) || !ensure_uniform_set(rd, pool)) return false;
	// The shadow pass and camera pass share this indirect-argument buffer. Upload the full
	// drawable set before recording either draw list; the camera cull must not erase it first.
	pool.upload_draw_args(pages);
	const int64_t dl = rd->draw_list_begin(c.framebuffer, RenderingDevice::DRAW_CLEAR_DEPTH,
			PackedColorArray(), 0.0f);
	if (dl < 0) return false;
	rd->draw_list_bind_render_pipeline(dl, pipeline_);
	rd->draw_list_bind_uniform_set(dl, set_.id(), 0);
	rd->draw_list_bind_index_array(dl, raster.index_array());
	ve::SunShadowPush push{};
	std::memcpy(push.sun_view_proj, ortho.view_proj, sizeof(push.sun_view_proj));
	rd->draw_list_set_push_constant(dl, gpu::push_bytes(push), sizeof(push));
	rd->draw_list_draw_indirect(dl, true, pool.args_buffer(), 0, static_cast<int>(pages.size()), 20);
	rd->draw_list_end();
	std::memcpy(c.view_proj, ortho.view_proj, sizeof(c.view_proj));
	c.texel_world = ortho.texel_world;
	c.depth_range = ortho.depth_range;
	c.dirty = false;
	c.frames_since = 0;
	c.last_pages = static_cast<int>(pages.size());
	c.rebuilds++;
	return true;
}
