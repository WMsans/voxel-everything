#include "world/material_table.h"

#include <cctype>
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

std::string upper(const char *name) {
	std::string s(name);
	for (char &c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

} // namespace

namespace ve {

float material_hardness(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 1.0f;
	return kMaterials[i].hardness;
}

float removal_radius(float nominal_radius, uint16_t material) {
	// material_hardness is floored at 1.0 (static_asserted in the header), so the effective
	// radius is never larger than the nominal one and op_world_aabb stays a true bound.
	return nominal_radius / material_hardness(material);
}

float material_glow(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i >= 0 && i < kMaterialCount) return kMaterials[i].glow;
	const int j = static_cast<int>(id) - static_cast<int>(kFoliageBase);
	return (j >= 0 && j < kFoliageCount) ? kFoliage[j].glow : 0.0f;
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

	o << "// Foliage (ve::kFoliage): ids FOLIAGE_BASE + k. No atlas layer and no flat albedo -- a\n"
	     "// foliage raster writes its own colour -- so only the emission tables exist.\n"
	     "const uint FOLIAGE_BASE = " << kFoliageBase << "u;\n"
	     "const int FOLIAGE_COUNT = " << kFoliageCount << ";\n";
	for (int k = 0; k < kFoliageCount; k++)
		o << "const uint MAT_" << upper(kFoliage[k].name) << " = " << (kFoliageBase + k) << "u;\n";
	o << "\n";

	o << "const float FOLIAGE_GLOW[FOLIAGE_COUNT] = float[FOLIAGE_COUNT](\n";
	for (int k = 0; k < kFoliageCount; k++)
		o << "\t" << f(kFoliage[k].glow) << (k + 1 < kFoliageCount ? "," : "")
		  << " // " << kFoliage[k].name << "\n";
	o << ");\n\n";

	o << "const vec3 FOLIAGE_GLOW_RGB[FOLIAGE_COUNT] = vec3[FOLIAGE_COUNT](\n";
	for (int k = 0; k < kFoliageCount; k++)
		o << "\t" << vec3(kFoliage[k].glow_rgb) << (k + 1 < kFoliageCount ? "," : "")
		  << " // " << kFoliage[k].name << "\n";
	o << ");\n\n";

	o << "// Mirror of ve::material_glow, including its fail-soft rule: air and any id with\n"
	     "// no table entry emit nothing. There is deliberately no hardness here -- a removal's\n"
	     "// size is resolved once on the CPU by ve::removal_radius before the op is uploaded,\n"
	     "// so every field sample sees one shape and no shader ever looks hardness up.\n"
	     "float mat_glow(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\tif (i >= 0 && i < MATERIAL_COUNT) return MAT_GLOW[i];\n"
	     "\tint j = int(id) - int(FOLIAGE_BASE);\n"
	     "\treturn (j >= 0 && j < FOLIAGE_COUNT) ? FOLIAGE_GLOW[j] : 0.0;\n"
	     "}\n\n"
	     "vec3 mat_glow_rgb(uint id) {\n"
	     "\tint i = int(id) - 1;\n"
	     "\tif (i >= 0 && i < MATERIAL_COUNT) return MAT_GLOW_RGB[i];\n"
	     "\tint j = int(id) - int(FOLIAGE_BASE);\n"
	     "\treturn (j >= 0 && j < FOLIAGE_COUNT) ? FOLIAGE_GLOW_RGB[j] : vec3(0.0);\n"
	     "}\n";
	return o.str();
}

} // namespace ve
