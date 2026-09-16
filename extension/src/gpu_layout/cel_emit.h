#pragma once
// ve::CelParams' defaults as GLSL constants for the compute shaders (shade.glslh) and as Godot
// shader-language constants for cel-shaded objects (cel.gdshaderinc), so the three copies of
// the ramp -- C++, GLSL, gdshader -- are one.
#include <string>

namespace ve::layout {

// The shortest %g form that parses back to exactly `v`, always containing '.', 'e' or 'E'.
std::string glsl_float(float v);

std::string cel_glsl();        // shaders/generated/cel.glslh
std::string cel_gdshaderinc(); // shaders/generated/cel_constants.gdshaderinc

} // namespace ve::layout
