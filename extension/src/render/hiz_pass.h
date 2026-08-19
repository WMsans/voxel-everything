#pragma once
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include "lod/lod_tree.h"
#include "render/async_readback.h"
#include <array>

namespace godot {

// Builds a fixed 256^2 R32F min-pyramid from the near-field scene depth and keeps a 32^2
// single-mip copy for the CPU octree occlusion test. Reverse-Z: near = 1.0, far = 0.0, so
// the conservative reduction is a MIN (it keeps the FARTHEST of the nearest surfaces over a
// footprint). The async readback is stale by design; the safe answer with no data is
// "visible", so stale occlusion can delay a build but never hide a chunk.
class HizPass {
public:
	static constexpr int kSize = 256;
	static constexpr int kMipCount = 9;
	static constexpr int kReadbackLevel = 3;
	static constexpr int kGrid = 32;

	HizPass() = default;
	~HizPass();

	bool initialize(RenderingDevice *rd);
	void teardown();
	// Records the full pyramid build (nine dispatches), the 32^2 copy, and (unless one is
	// already in flight) the async readback request. The caller owns command submission.
	bool build(RenderingDevice *rd, RID scene_depth, Vector2i scene_size);

	RID pyramid() const { return pyramid_; }
	int mip_count() const { return kMipCount; }
	const ve::LodOcclusion *occlusion() const { return &occlusion_; }

	// Debug/test support used by VoxelWorld::debug_hiz_*.
	RID readback_texture() const { return readback_tex_; }
	bool readback_pending() const { return readback_.is_valid() && readback_->pending(); }
	bool readback_was_pending_at_teardown() const { return readback_was_pending_at_teardown_; }
	bool readback_was_drained_at_teardown() const { return readback_was_drained_at_teardown_; }
	// Drops the cached level-0 uniform set before an external scene-depth source is freed.
	// Freeing a texture cascades to referencing uniform sets, so the cache must not try to
	// free the already-cascade-freed set on the next build.
	void release_level0_set();
	bool update_occlusion(const PackedByteArray &data);
	// Synchronous read of one texel from a specific pyramid mip. Caller must have submitted
	// and synced the device after the build that produced the data.
	float probe_mip_texel(RenderingDevice *rd, int level, int x, int y) const;
	int size_at(int level) const;

private:
	class HizOcclusion : public ve::LodOcclusion {
	public:
		void update(const PackedByteArray &data);
		bool occluded(const float ss_min[3], const float ss_max[3]) const override;
		bool have_data() const { return have_data_; }

	private:
		bool have_data_ = false;
		float grid_[kGrid * kGrid] = {};
	};

	bool ensure_uniform_set(RenderingDevice *rd, RID src, int dst_mip);

	RenderingDevice *rd_ = nullptr;
	RID shader_, pipeline_, sampler_;
	RID pyramid_, readback_tex_;
	std::array<RID, kMipCount> slices_{};
	std::array<RID, kMipCount> usets_{};
	RID uset0_src_;
	HizOcclusion occlusion_;
	Ref<AsyncTextureRead> readback_;
	bool readback_was_pending_at_teardown_ = false;
	bool readback_was_drained_at_teardown_ = true;
};

} // namespace godot
