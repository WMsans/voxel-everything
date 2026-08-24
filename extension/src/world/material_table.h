#pragma once
#include <cstdint>
#include <string>

namespace ve {

// The authoritative material definition. Material id i + 1 is served by atlas layer i;
// material 0 is air and has no layer. This table is mirrored into GLSL as
// shaders/material_table.glslh (see material_table_glsl(), gated by a byte-exact test)
// and into GDScript via VoxelWorld::material_table().
//
// `asset` is the two-digit prefix of this material's PNGs under assets/materials/, e.g.
// "01" for 01_basecolor.png. It is a string rather than an index because the layer order
// and the on-disk numbering are allowed to be read independently by tools/convert_materials.sh.
struct MaterialDef {
	const char *name;      // picker label
	const char *asset;     // "04" -> assets/materials/04_basecolor.png, ...
	float hardness;        // >= 1.0; divides a carve's radius. 1.0 = full radius.
	float glow;            // emissive strength; 0.0 = not emissive
	float glow_rgb[3];
	float flat_albedo[3];  // far-field and unknown-layer fallback
};

// Order IS atlas layer order, and must match MATERIALS in tools/convert_materials.sh.
inline constexpr MaterialDef kMaterials[] = {
	// name          asset  hardness glow  glow_rgb              flat_albedo
	{"grass_01",     "00",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.36f, 0.55f, 0.22f}},
	{"rock",         "01",  3.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.45f, 0.42f, 0.40f}},
	{"ground_01",    "02",  1.4f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.50f, 0.35f, 0.20f}},
	{"breakstone",   "03",  2.2f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.62f, 0.60f, 0.66f}},
};

inline constexpr int kMaterialCount = static_cast<int>(sizeof(kMaterials) / sizeof(kMaterials[0]));

// A hardness below 1.0 would let a carve reach past op_world_aabb's pos +/- radius. Ops
// that reach outside their declared AABB are dropped at region boundaries, so this is a
// silent-data-loss bug, not a visual one. Caught at compile time instead.
constexpr bool material_hardness_floor_holds() {
	for (int i = 0; i < kMaterialCount; i++)
		if (!(kMaterials[i].hardness >= 1.0f)) return false;
	return true;
}
static_assert(material_hardness_floor_holds(), "material hardness must be >= 1.0");

// Fail soft for air (0) and any id with no table entry: full-radius carve, no emission.
float material_hardness(uint16_t id);
float material_glow(uint16_t id);

// The exact intended contents of shaders/material_table.glslh. That file is committed and a
// unit test asserts it equals this string byte for byte; the test prints this text on
// failure, so regenerating is copy-and-paste. Emitting at runtime and registering through
// load_shader_source's override map was rejected: BrickGenPass compiles brick_gen.comp.glsl
// at render/orchestrator.cpp:182, before MaterialAtlas::initialize at :184, and
// clear_shader_source_overrides() (called by tests) would leave the include unresolvable.
std::string material_table_glsl();

} // namespace ve
