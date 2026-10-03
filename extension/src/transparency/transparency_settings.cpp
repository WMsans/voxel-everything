#include "transparency/transparency_settings.h"

namespace ve {
namespace {

const SettingRow<TransparencySettings> kTransparencyRows[] = {
	bool_row("enabled", "Transparency", &TransparencySettings::enabled),
	// Capped below 1: at 1 the medium never tints and thickness is invisible.
	float_row("min_transmit", "Min transmit", &TransparencySettings::min_transmit, 0.0f, 0.95f,
			0.0f, 0.95f, 0.01f),
	float_row("sky_thickness_m", "Sky thickness (m)", &TransparencySettings::sky_thickness_m,
			0.0f, 50.0f, 0.0f, 20.0f, 0.1f),
};

} // namespace

std::span<const SettingRow<TransparencySettings>> transparency_rows() { return kTransparencyRows; }

void clamp_transparency_settings(TransparencySettings *s) {
	clamp_all<TransparencySettings>(kTransparencyRows, s);
}

} // namespace ve
