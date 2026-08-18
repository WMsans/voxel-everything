#include "render/material_atlas.h"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>

using namespace godot;

namespace {

// Index order IS the layer order used by tools/convert_materials.sh. Layer i serves ve
// material id i + 1; material 0 is air and has no layer.
const char *kMaterialNames[] = {"grass_01", "rock", "ground_01", "breakstone"};

PackedByteArray load_png(const String &path) {
	return FileAccess::get_file_as_bytes(path);
}

bool decode_png(const PackedByteArray &bytes, Ref<Image> *out) {
	Ref<Image> img;
	img.instantiate();
	if (img->load_png_from_buffer(bytes) != OK) return false;
	if (img->get_width() != kMaterialTextureSize || img->get_height() != kMaterialTextureSize) return false;
	img->convert(Image::FORMAT_RGBA8);
	*out = img;
	return true;
}

// Builds one 512^2 RGBA8 image with all mips. `albedo` selects which of the two array
// formats is being packed; the five input images must be decoded RGBA8 512^2.
PackedByteArray pack_layer(bool albedo, const std::array<Ref<Image>, 5> &maps) {
	PackedByteArray data;
	data.resize(kMaterialTextureSize * kMaterialTextureSize * 4);
	uint8_t *dst = data.ptrw();
	const PackedByteArray base = maps[0]->get_data();
	const PackedByteArray normal = maps[1]->get_data();
	const PackedByteArray rough = maps[2]->get_data();
	const PackedByteArray ao = maps[3]->get_data();
	const PackedByteArray height = maps[4]->get_data();
	const uint8_t *bp = base.ptr();
	const uint8_t *np = normal.ptr();
	const uint8_t *rp = rough.ptr();
	const uint8_t *ap = ao.ptr();
	const uint8_t *hp = height.ptr();
	for (int y = 0; y < kMaterialTextureSize; y++) {
		for (int x = 0; x < kMaterialTextureSize; x++) {
			const int64_t o = (static_cast<int64_t>(y) * kMaterialTextureSize + x) * 4;
			uint8_t *p = dst + o;
			if (albedo) {
				// albedo <- basecolor RGB, height in A.
				p[0] = bp[o + 0];
				p[1] = bp[o + 1];
				p[2] = bp[o + 2];
				p[3] = hp[o + 0];
			} else {
				// surface <- normal XY, roughness in B, AO in A.
				p[0] = np[o + 0];
				p[1] = np[o + 1];
				p[2] = rp[o + 0];
				p[3] = ap[o + 0];
			}
		}
	}
	Ref<Image> img = Image::create_from_data(kMaterialTextureSize, kMaterialTextureSize, false,
			Image::FORMAT_RGBA8, data);
	img->generate_mipmaps();
	return img->get_data();
}

PackedByteArray flat_layer(bool albedo) {
	PackedByteArray data;
	data.resize(kMaterialTextureSize * kMaterialTextureSize * 4);
	uint8_t *p = data.ptrw();
	for (int i = 0; i < kMaterialTextureSize * kMaterialTextureSize; i++) {
		uint8_t *px = p + i * 4;
		if (albedo) {
			px[0] = 255; px[1] = 0; px[2] = 255; px[3] = 0; // error magenta, height 0
		} else {
			px[0] = 128; px[1] = 128; px[2] = 255; px[3] = 255; // neutral normal/white AO
		}
	}
	Ref<Image> img = Image::create_from_data(kMaterialTextureSize, kMaterialTextureSize, false,
			Image::FORMAT_RGBA8, data);
	img->generate_mipmaps();
	return img->get_data();
}

} // namespace

MaterialAtlas::~MaterialAtlas() {
	teardown();
}

bool MaterialAtlas::initialize(RenderingDevice *rd) {
	teardown();
	rd_ = rd;
	if (!rd) return false;

	TypedArray<PackedByteArray> albedo_data;
	TypedArray<PackedByteArray> surface_data;
	const int source_count = static_cast<int>(std::size(kMaterialNames));
	for (int layer = 0; layer < kMaterialLayers; layer++) {
		if (layer < source_count) {
			std::array<Ref<Image>, 5> maps;
			const char *suffixes[] = {"basecolor", "normal", "roughness",
					"ambientOcclusion", "height"};
			for (int m = 0; m < 5; m++) {
				char rel[256];
				std::snprintf(rel, sizeof(rel), "res://assets/materials/%02d_%s.png", layer,
						suffixes[m]);
				const String path = ProjectSettings::get_singleton()->globalize_path(rel);
				PackedByteArray bytes = load_png(path);
				if (!decode_png(bytes, &maps[m])) {
					UtilityFunctions::printerr("MaterialAtlas: failed to load ", path);
					teardown();
					return false;
				}
			}
			albedo_data.push_back(pack_layer(true, maps));
			surface_data.push_back(pack_layer(false, maps));
		} else {
			albedo_data.push_back(flat_layer(true));
			surface_data.push_back(flat_layer(false));
		}
	}

	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	f->set_format(RenderingDevice::DATA_FORMAT_R8G8B8A8_UNORM);
	f->set_width(kMaterialTextureSize);
	f->set_height(kMaterialTextureSize);
	f->set_array_layers(kMaterialLayers);
	f->set_mipmaps(kMaterialMipmaps);
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	albedo_array_ = rd->texture_create(f, v, albedo_data);
	surface_array_ = rd->texture_create(f, v, surface_data);

	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	ss->set_mip_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	ss->set_repeat_u(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	ss->set_repeat_v(RenderingDevice::SAMPLER_REPEAT_MODE_REPEAT);
	ss->set_repeat_w(RenderingDevice::SAMPLER_REPEAT_MODE_CLAMP_TO_EDGE);
	sampler_ = rd->sampler_create(ss);

	if (!albedo_array_.is_valid() || !surface_array_.is_valid() || !sampler_.is_valid()) {
		UtilityFunctions::printerr("MaterialAtlas: texture/sampler creation failed");
		teardown();
		return false;
	}
	return true;
}

void MaterialAtlas::teardown() {
	if (!rd_) return;
	if (albedo_array_.is_valid()) rd_->free_rid(albedo_array_);
	if (surface_array_.is_valid()) rd_->free_rid(surface_array_);
	if (sampler_.is_valid()) rd_->free_rid(sampler_);
	albedo_array_ = RID();
	surface_array_ = RID();
	sampler_ = RID();
	rd_ = nullptr;
}
