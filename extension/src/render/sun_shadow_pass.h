#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "shade/sun_ortho.h"

namespace godot {

class LodPool;
class LodRasterPass;

class SunShadowPass {
public:
	static constexpr int kSize = 2048;
	static constexpr int kMinFrames = 12;

	~SunShadowPass();
	bool initialize(RenderingDevice *rd);
	void teardown();
	void mark_dirty();
	bool build(RenderingDevice *rd, LodPool &pool, LodRasterPass &raster,
			const ve::SunOrtho &ortho, bool force);
	RID map() const { return map_; }
	const float *view_proj() const { return view_proj_; }
	float texel_world() const { return texel_world_; }
	int rebuilds() const { return rebuilds_; }
	bool is_valid() const { return rd_ && map_.is_valid() && shader_.is_valid(); }

private:
	bool ensure_pipeline(RenderingDevice *rd, LodRasterPass &raster);
	bool ensure_uniform_set(RenderingDevice *rd, LodPool &pool);

	RenderingDevice *rd_ = nullptr;
	RID map_;
	RID framebuffer_;
	RID shader_;
	RID pipeline_;
	RID uset_;
	RID key_quads_;
	RID key_page_chunk_;
	RID key_chunks_;
	bool pipeline_front_face_clockwise_ = false;
	bool pipeline_front_face_set_ = false;
	bool dirty_ = true;
	int frames_since_ = 0;
	int rebuilds_ = 0;
	float view_proj_[16] = {};
	float texel_world_ = 0.0f;
};

} // namespace godot
