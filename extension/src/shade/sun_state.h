#pragma once
#include "shade/cel.h"

namespace ve {

// Everything the renderer needs to know about the sun this frame. Resolved from the scene's
// DirectionalLight3D on the main thread and published to the GPU through one UBO.
//
// `dir` points TOWARD the sun, matching kSunDir and sun_ortho's contract. A DirectionalLight3D
// emits along its local -Z, so `dir` is +basis.column(2).
struct SunState {
	float dir[3] = {kSunDir[0], kSunDir[1], kSunDir[2]}; // normalized, toward the sun
	float right[3] = {}; // the light's basis X in world space; all-zero when unauthored
	float up[3] = {};    // the light's basis Y in world space; all-zero when unauthored
	float rgb[3] = {1.0f, 1.0f, 1.0f}; // linear light_color * light_energy

	// An all-zero basis is the explicit signal for "no light node resolved". Callers select
	// sun_ortho's direction-only overload, which derives a basis as it always has. A zero
	// basis must never reach the explicit-basis overload.
	bool has_basis() const;
};

// The standard sRGB electro-optical transfer function. DirectionalLight3D::get_color() is
// sRGB as authored in the inspector; this engine shades in linear.
float srgb_to_linear(float c);

} // namespace ve
