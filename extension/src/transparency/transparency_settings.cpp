#include "transparency/transparency_settings.h"

namespace ve {
namespace {

const SettingRow<TransparencySettings> kTransparencyRows[] = {
	bool_row("enabled", "Transparency", &TransparencySettings::enabled),
	// Floored above zero: a zero step never advances, and the walk would spend its whole
	// cap on one sample.
	float_row("min_step_m", "Min step (m)", &TransparencySettings::min_step_m, 0.01f, 1.0f,
			0.01f, 0.5f, 0.01f),
	int_row("max_steps", "Max steps", &TransparencySettings::max_steps, 1, 256, 1, 128),
	float_row("min_transmit", "Min transmit", &TransparencySettings::min_transmit, 0.0f, 0.5f,
			0.0f, 0.1f, 0.001f),
};

} // namespace

std::span<const SettingRow<TransparencySettings>> transparency_rows() { return kTransparencyRows; }

void clamp_transparency_settings(TransparencySettings *s) {
	clamp_all<TransparencySettings>(kTransparencyRows, s);
}

} // namespace ve
