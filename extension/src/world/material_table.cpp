#include "world/material_table.h"

#include <cstdio>
#include <sstream>

namespace {

// Fixed 6-decimal form so the emitted text is byte-stable across platforms and locales
// -- the mirror test compares strings, not floats.
std::string f(float v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.6f", static_cast<double>(v));
	return buf;
}

std::string vec3(const float v[3]) {
	return "vec3(" + f(v[0]) + ", " + f(v[1]) + ", " + f(v[2]) + ")";
}

} // namespace

namespace ve {

float material_hardness(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 1.0f;
	return kMaterials[i].hardness;
}

float material_glow(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 0.0f;
	return kMaterials[i].glow;
}

std::string material_table_glsl() {
	std::ostringstream o;
	o << "// GENERATED from extension/src/world/material_table.h (ve::kMaterials) by\n"
	     "// ve::material_table_glsl(). Do not edit by hand: extension/tests/"
	     "test_material_glslh.cpp\n"
	     "// asserts this file byte for byte and prints the correct contents on failure.\n"
	     "//\n"
	     "// Every array is indexed by MATERIAL ID MINUS ONE. Id 0 is air and has no entry.\n"
	     "// NOTE: never put a literal include directive inside a comment in this file -- the\n"
	     "// loader matches include tokens anywhere in a line and would self-include.\n"
	     "\n"
	     "const int MATERIAL_COUNT = " << kMaterialCount << ";\n\n";

	o << "const float MAT_HARDNESS[MATERIAL_COUNT] = float[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << f(kMaterials[i].hardness) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const float MAT_GLOW[MATERIAL_COUNT] = float[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << f(kMaterials[i].glow) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const vec3 MAT_GLOW_RGB[MATERIAL_COUNT] = vec3[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << vec3(kMaterials[i].glow_rgb) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "const vec3 MAT_FLAT_ALBEDO[MATERIAL_COUNT] = vec3[MATERIAL_COUNT](\n";
	for (int i = 0; i < kMaterialCount; i++)
		o << "\t" << vec3(kMaterials[i].flat_albedo) << (i + 1 < kMaterialCount ? "," : "")
		  << " // " << kMaterials[i].name << "\n";
	o << ");\n\n";

	o << "// Mirrors of ve::material_hardness / ve::material_glow, including their fail-soft\n"
	     "// rule: air and any id with no table entry carve at full radius and emit nothing.\n"
	     "float mat_hardness(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i < 0 || i >= MATERIAL_COUNT) ? 1.0 : MAT_HARDNESS[i];\n"
	     "}\n\n"
	     "float mat_glow(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i < 0 || i >= MATERIAL_COUNT) ? 0.0 : MAT_GLOW[i];\n"
	     "}\n\n"
	     "vec3 mat_glow_rgb(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\treturn (i < 0 || i >= MATERIAL_COUNT) ? vec3(0.0) : MAT_GLOW_RGB[i];\n"
	     "}\n";
	return o.str();
}

} // namespace ve
