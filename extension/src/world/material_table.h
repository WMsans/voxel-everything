#pragma once
#include <cstdint>
#include <string>
#include <string_view>

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
	float hardness;        // >= 1.0; divides a removal's nominal size. 1.0 = full size.
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
	{"ground_crack_01", "04", 1.1f, 6.0f, {1.00f, 0.35f, 0.08f}, {0.35f, 0.12f, 0.06f}},
	{"ice_crack",    "05",  1.8f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.61f, 0.65f, 0.68f}},
	{"ice",          "06",  1.8f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.61f, 0.65f, 0.68f}},
};

inline constexpr int kMaterialCount = static_cast<int>(sizeof(kMaterials) / sizeof(kMaterials[0]));

// Layers in the material texture arrays (render/material_atlas.h allocates exactly this many and
// fills unused layers with flat error magenta); generated into GLSL as MATERIAL_LAYERS.
inline constexpr int kMaterialLayers = 16;
static_assert(kMaterialCount <= kMaterialLayers, "more materials than atlas layers");

// The id of the terrain material called `name`. Code that places a material by name (terrain
// stages, the analytic generator, grass) uses this, never a literal, so reordering kMaterials
// renumbers every use at once. An unknown name is a compile error wherever the result must be
// a constant expression (it reaches a non-constexpr call), and air (0) at run time.
uint16_t unknown_material_name();

constexpr uint16_t material_id(std::string_view name) {
	for (int i = 0; i < kMaterialCount; i++)
		if (name == kMaterials[i].name) return static_cast<uint16_t>(i + 1);
	return unknown_material_name();
}

// Foliage draws its own albedo and grows ON terrain rather than being terrain, so it has no
// atlas layer, no flat albedo and no hardness. Its ids start at kFoliageBase, far above every
// terrain id: adding a terrain material never renumbers foliage, and no foliage id can reach
// an atlas lookup (every GLSL table lookup is range-checked). A new grass or leaf type is one
// row here plus its shader.
struct FoliageDef {
	const char *name;
	float glow; // emissive strength; 0.0 = not emissive
	float glow_rgb[3];
};

inline constexpr uint16_t kFoliageBase = 200;

inline constexpr FoliageDef kFoliage[] = {
	// name          glow  glow_rgb
	{"grass_blade",  0.0f, {0.0f, 0.0f, 0.0f}},
};

inline constexpr int kFoliageCount = static_cast<int>(sizeof(kFoliage) / sizeof(kFoliage[0]));
static_assert(kMaterialCount < kFoliageBase, "terrain material ids would reach the foliage range");

// Hardness models RESISTANCE: it may shrink a removal's nominal dimensions, never enlarge
// them. Softness is expressed by authoring a larger tool radius, not by a hardness below
// one. Caught at compile time so the table cannot express the other direction by accident.
constexpr bool material_hardness_floor_holds() {
	for (int i = 0; i < kMaterialCount; i++)
		if (!(kMaterials[i].hardness >= 1.0f)) return false;
	return true;
}
static_assert(material_hardness_floor_holds(), "material hardness must be >= 1.0");

// Fail soft for air (0) and any id with no table entry: full-size removal, no emission.
// material_glow also answers for foliage ids (kFoliageBase + k).
float material_hardness(uint16_t id);
float material_glow(uint16_t id);

// The effective size of a removal that a ray struck on `material`. Hardness is resolved
// EXACTLY ONCE, here, before the op reaches any field evaluator: ve::apply_op and
// shaders/field.glslh then see one ordinary sphere whose shape does not depend on the
// material at the sample point. Scaling per sample instead kept the field's sign right but
// destroyed its magnitude as a distance bound at a seam, which the near-field marcher
// stepped straight through.
//
// The result is the op's EXACT geometric reach, so op_world_aabb (pos +/- radius) stays
// tight and every consumer built on it -- region ranges, brick residency, connectivity
// re-marking, op filtering -- agrees with what the evaluator draws.
//
// A future non-spherical removal scales its own dimensions through this same call; no field
// evaluator gains a material branch.
float removal_radius(float nominal_radius, uint16_t material);

// The exact intended contents of shaders/material_table.glslh. Hardness is deliberately
// NOT emitted: it is consumed once on the CPU at op construction (see removal_radius), so
// no shader needs the table or a lookup. The file is committed and a unit test asserts it
// equals this string byte for byte; the test prints this text on failure, so regenerating
// is copy-and-paste. Emitting at runtime and registering through
// load_shader_source's override map was rejected: BrickGenPass compiles brick_gen.comp.glsl
// at render/orchestrator.cpp:182, before MaterialAtlas::initialize at :184, and
// clear_shader_source_overrides() (called by tests) would leave the include unresolvable.
std::string material_table_glsl();

} // namespace ve
