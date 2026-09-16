#include "settings/render_settings.h"

namespace ve {
namespace {

const char *const kQualityTiers[] = {"Off", "Low", "Medium", "High"};

const SettingRow<RenderSettings> kRenderRows[] = {
	enum_row("quality_tier", "Quality", &RenderSettings::quality_tier,
			std::span<const char *const>(kQualityTiers)),
	float_row("near_field_scale", "Near-field resolution", &RenderSettings::near_field_scale, 0.1f,
			1.0f, 0.1f, 1.0f, 0.01f,
			"Near-field resolution is the fraction of the internal 3D resolution the raymarcher runs "
			"at. Only the terrain SILHOUETTE is resolved on that coarser grid, which is what the "
			"stepped pixels along terrain edges are; the material texture is resolved per "
			"full-resolution pixel either way, so lowering this does not blur the ground. Raise it "
			"to lose the stepping, lower it to buy frame time."),
	bool_row("near_field", "Near field", &RenderSettings::near_field),
	bool_row("islands", "Islands", &RenderSettings::islands),
};

} // namespace

std::span<const SettingRow<RenderSettings>> render_rows() {
	return kRenderRows;
}

} // namespace ve
