#pragma once
#include "settings/settings_table.h"
#include <span>

namespace ve {

// Transparent-material knobs (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md
// §8). DELIBERATELY its own module and store, like grass and leaves: nothing here joins
// BeautySettings. What a material lets through is a material-table column, not a knob here.
struct TransparencySettings {
	// A STARTUP switch for the opaque-view bake, observed LIVE for the near shell. Lattices
	// are baked where data is generated -- brick generation and island upload -- and are NOT
	// re-baked on a toggle, so turning this off mid-session leaves existing bricks and
	// islands marching as solids (and turning it back on leaves them marching as air). The
	// near shell's candidate set and pages DO follow a toggle: LodSystem::refresh_shell_
	// candidates forces a recompute, so the shell is torn down and rebuilt immediately. Off
	// from a cold start: opaque_view is the identity, no shell is built or drawn, and
	// transparent materials render opaque.
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
