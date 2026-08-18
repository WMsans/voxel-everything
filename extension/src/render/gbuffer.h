#pragma once
#include <godot_cpp/classes/render_scene_buffers_rd.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>

namespace godot {

// Spec section 7's merged G-buffer. Two colour attachments and a depth attachment this
// engine owns, plus the deferred stack's output and the half-resolution copy of last
// frame's finished image that SSGI bounces light from.
//
//   albedo   R8G8B8A8_UNORM        rgb = albedo, a = sun visibility (shadow layer 1 and 2)
//   surface  R16G16B16A16_SFLOAT   xy = oct normal, z = material id, w = gloss
//   depth    D32_SFLOAT            reverse-Z, the SAME NDC as Godot's scene depth, so the
//                                  injection is a copy rather than a reprojection
//   lit      R16G16B16A16_SFLOAT   what the deferred pass produced this frame
//   history  R16G16B16A16_SFLOAT   HALF resolution; last frame's finished scene colour
//
// There is no separate linear-depth target (spec section 7 lists one). Linear depth is
// reconstructed from `depth` and the projection, which is exact, costs two ALU, and cannot
// disagree with the depth the raster actually tested against.
class GBuffer {
public:
	static const char *kContext; // "voxel_gbuf"

	~GBuffer();

	// `rsb` may be null. Non-null is the production path: the textures are named entries on
	// Godot's render scene buffers, so a viewport reconfigure frees them and the next
	// ensure() recreates them with no resize handling here. Null is the probe path: plain
	// RD textures this object owns and frees.
	bool ensure(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size);
	void teardown();

	bool is_valid() const;
	Vector2i size() const { return size_; }
	Vector2i half_size() const;

	RID albedo() const { return albedo_; }
	RID surface() const { return surface_; }
	RID depth() const { return depth_; }
	RID lit() const { return lit_; }
	RID history() const { return history_; }

	// Diagnostic: how many times ensure() has had to allocate. A number that climbs every
	// frame means the size or the context is churning.
	int reallocations() const { return reallocations_; }

private:
	bool ensure_owned(RenderingDevice *rd, Vector2i size);
	bool ensure_managed(RenderingDevice *rd, RenderSceneBuffersRD *rsb, Vector2i size);
	void free_owned();

	RenderingDevice *rd_ = nullptr;
	bool owned_ = false;
	Vector2i size_{0, 0};
	int reallocations_ = 0;
	RID albedo_, surface_, depth_, lit_, history_;
};

} // namespace godot
