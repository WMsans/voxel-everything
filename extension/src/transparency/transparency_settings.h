#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Transparent-material knobs (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md
// §8). DELIBERATELY its own module and store, like grass and leaves: nothing here joins
// BeautySettings. What a material lets through is a material-table column, not a knob here.
struct TransparencySettings {
	// Read where lattices are BAKED (brick generation, island upload) and where shells are
	// built, so it takes effect for data produced afterwards: a startup and benchmark A/B
	// switch, not a live toggle. Off: opaque_view is the identity, no shell is built or
	// drawn, and transparent materials render opaque.
	bool enabled = true;
	// The transmittance floor, per channel: however thick the medium, at least this much of
	// what is behind shows through.
	float min_transmit = 0.35f;
	// Thickness assumed past the nearest front when a ray enters the medium, finds no exit
	// face and has only sky behind it (a shell cut off at a streaming edge).
	float sky_thickness_m = 4.0f;
};

std::span<const SettingRow<TransparencySettings>> transparency_rows();

// Pulls every field into its documented range, NaN included. Idempotent.
void clamp_transparency_settings(TransparencySettings *s);

} // namespace ve
