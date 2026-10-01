#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Transparent-material knobs (docs/superpowers/specs/2026-09-29-transparent-materials-design.md
// §8). DELIBERATELY its own module and store, like grass and leaves: nothing here joins
// BeautySettings. What a material lets through is a material-table column, not a knob here.
struct TransparencySettings {
	// Off: the marcher hits transparent materials as opaque, the far shell and the composite
	// do not run, and the near field renders exactly as it did before transparency existed.
	// Also the benchmark's A/B switch.
	bool enabled = true;
	// The walk's smallest step, metres. The real step is max(this, the pixel footprint at
	// that distance), so 5 cm is paid only close to the camera (spec §4).
	float min_step_m = 0.05f;
	// Safety cap on walk steps. Hitting it means the medium counts as fully absorbed.
	int max_steps = 48;
	// The walk ends as soon as the brightest channel of the transmittance drops under this:
	// nothing behind is visible any more, so nothing behind is marched.
	float min_transmit = 0.01f;
};

std::span<const SettingRow<TransparencySettings>> transparency_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_transparency_settings(TransparencySettings *s);

} // namespace ve
