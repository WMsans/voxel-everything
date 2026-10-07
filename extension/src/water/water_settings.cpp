#include "water/water_settings.h"

namespace ve {
namespace {

const SettingRow<WaterSettings> kWaterRows[] = {
	float_row("wave_strength", "Wave strength", &WaterSettings::wave_strength, 0.0f, 3.0f,
			0.0f, 3.0f, 0.05f),
	float_row("flow_speed", "Flow speed (m/s)", &WaterSettings::flow_speed, 0.0f, 5.0f,
			0.0f, 3.0f, 0.05f),
	float_row("refraction_strength", "Refraction", &WaterSettings::refraction_strength, 0.0f,
			0.2f, 0.0f, 0.1f, 0.005f),
	float_row("foam_width_m", "Foam width (m)", &WaterSettings::foam_width_m, 0.0f, 3.0f,
			0.0f, 1.5f, 0.05f),
};

} // namespace

std::span<const SettingRow<WaterSettings>> water_rows() { return kWaterRows; }

void clamp_water_settings(WaterSettings *s) {
	clamp_all<WaterSettings>(kWaterRows, s);
}

} // namespace ve
