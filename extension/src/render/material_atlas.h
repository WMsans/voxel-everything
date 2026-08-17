#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace godot {

// The texture-array layer count is fixed at compile time and mirrored in every shader that
// samples the arrays as MATERIAL_LAYERS. It is a compile-time bound on an array index, so an
// out-of-range material id is a cheap branch instead of undefined behaviour. MaterialAtlas
// always allocates this many layers and fills unused ones with flat error magenta, so the
// constant and the texture can never disagree.
constexpr int kMaterialLayers = 16;
constexpr int kMaterialTextureSize = 512;
constexpr int kMaterialMipmaps = 10; // floor(log2(512)) + 1

// Owns the two 512^2 RGBA8 2D-array textures that give every material its basecolor/height
// and normal/roughness/AO. This is Godot glue only: it reads PNGs through Godot's Image,
// packs channels, and hands the concatenated mip chains to RenderingDevice::texture_create.
class MaterialAtlas {
public:
	~MaterialAtlas();

	bool initialize(RenderingDevice *rd);
	void teardown();
	bool is_valid() const { return albedo_array_.is_valid() && surface_array_.is_valid(); }

	RID albedo_array() const { return albedo_array_; }
	RID surface_array() const { return surface_array_; }
	RID sampler() const { return sampler_; }
	int layer_count() const { return kMaterialLayers; }

private:
	RenderingDevice *rd_ = nullptr;
	RID albedo_array_, surface_array_, sampler_;
};

} // namespace godot
