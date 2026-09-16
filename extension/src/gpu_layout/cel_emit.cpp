#include "gpu_layout/cel_emit.h"
#include "shade/cel.h"
#include <cstdio>
#include <cstdlib>

namespace ve::layout {

namespace {

const char *kHeader =
		"// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
		"// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n";

std::string list(const float *v, int n) {
	std::string out;
	for (int i = 0; i < n; i++) out += (i ? ", " : "") + glsl_float(v[i]);
	return out;
}

} // namespace

std::string glsl_float(float v) {
	char buf[32];
	for (int digits = 6; digits <= 9; digits++) {
		std::snprintf(buf, sizeof(buf), "%.*g", digits, static_cast<double>(v));
		if (std::strtof(buf, nullptr) == v) break;
	}
	std::string s(buf);
	if (s.find_first_of(".eE") == std::string::npos) s += ".0";
	return s;
}

std::string cel_glsl() {
	const CelParams p;
	const std::string edges = std::to_string(kCelBands - 1);
	const std::string bands = std::to_string(kCelBands);
	return std::string("// GENERATED from extension/src/shade/cel.h (ve::CelParams) by ve::layout::cel_glsl().\n") +
			kHeader + "\n" +
			"const int CEL_BANDS = " + bands + ";\n" +
			"const float CEL_BAND_EDGE[" + edges + "] = float[" + edges + "](" + list(p.band_edge, kCelBands - 1) + ");\n" +
			"const float CEL_BAND_LEVEL[" + bands + "] = float[" + bands + "](" + list(p.band_level, kCelBands) + ");\n" +
			"const float CEL_SHADOW_HUE_SHIFT = " + glsl_float(p.shadow_hue_shift) + ";\n" +
			"const float CEL_SHADOW_SATURATION = " + glsl_float(p.shadow_saturation) + ";\n" +
			"const float CEL_SPEC_EDGE = " + glsl_float(p.spec_edge) + ";\n" +
			"const float CEL_SPEC_STRENGTH = " + glsl_float(p.spec_strength) + ";\n" +
			"const float CEL_RIM_STRENGTH = " + glsl_float(p.rim_strength) + ";\n" +
			"const float CEL_RIM_POWER = " + glsl_float(p.rim_power) + ";\n";
}

std::string cel_gdshaderinc() {
	const CelParams p;
	std::string out = std::string("// GENERATED from extension/src/shade/cel.h (ve::CelParams) by ve::layout::cel_gdshaderinc().\n") +
			kHeader + "\n";
	for (int i = 0; i < kCelBands - 1; i++)
		out += "const float VE_CEL_BAND_EDGE_" + std::to_string(i) + " = " + glsl_float(p.band_edge[i]) + ";\n";
	for (int i = 0; i < kCelBands; i++)
		out += "const float VE_CEL_BAND_LEVEL_" + std::to_string(i) + " = " + glsl_float(p.band_level[i]) + ";\n";
	out += "const float VE_HUE = " + glsl_float(p.shadow_hue_shift) + ";\n";
	out += "const float VE_SAT = " + glsl_float(p.shadow_saturation) + ";\n";
	out += "const float VE_SPEC_EDGE = " + glsl_float(p.spec_edge) + ";\n";
	out += "const float VE_SPEC = " + glsl_float(p.spec_strength) + ";\n";
	out += "const float VE_RIM = " + glsl_float(p.rim_strength) + ";\n";
	out += "const float VE_RIM_POWER = " + glsl_float(p.rim_power) + ";\n";
	return out;
}

} // namespace ve::layout
