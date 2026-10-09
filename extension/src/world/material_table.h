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
// Which liquid a material is (docs/superpowers/specs/2026-10-06-water-voxels-design.md §2).
// A liquid is also transparent (transmit > 0), so the shell, thickness and composite draw it;
// on top of that it is a GHOST: colliders, occupancy, contact refinement and island
// extraction treat it as air (the solid view, world/opaque_view.h). Only the transparency
// composite's shading branch asks WHICH liquid. A new liquid is one enum value, one name
// below, and its shading.
enum class Liquid : uint8_t { none = 0, water = 1 };

// GLSL constant names, indexed by the enum's value: LIQUID_<NAME>.
inline constexpr const char *kLiquidNames[] = {"none", "water"};
inline constexpr int kLiquidCount = static_cast<int>(sizeof(kLiquidNames) / sizeof(kLiquidNames[0]));
static_assert(kLiquidCount == static_cast<int>(Liquid::water) + 1, "a Liquid value has no name");

struct MaterialDef {
	const char *name;      // picker label
	const char *asset;     // "04" -> assets/materials/04_basecolor.png, ...
	float hardness;        // >= 1.0; divides a removal's nominal size. 1.0 = full size.
	float glow;            // emissive strength; 0.0 = not emissive
	float glow_rgb[3];
	float flat_albedo[3];  // far-field and unknown-layer fallback
	// Transparency (docs/superpowers/specs/2026-10-01-transparent-voxels-design.md §4).
	// The fraction of light per channel left after one metre of the material; {0,0,0} is
	// opaque, and that is what "transparent" means everywhere: any channel above zero.
	float transmit[3] = {0.0f, 0.0f, 0.0f};
	float ior = 1.0f;      // index of refraction, for Fresnel reflectance; unused when opaque
	Liquid liquid = Liquid::none; // which liquid shading and ghost rule apply; none = not a liquid
};

// Order IS atlas layer order, and must match MATERIALS in tools/convert_materials.sh.
inline constexpr MaterialDef kMaterials[] = {
	// name          asset  hardness glow  glow_rgb              flat_albedo
	{"grass_01",     "00",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.36f, 0.55f, 0.22f}},
	{"rock",         "01",  3.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.45f, 0.42f, 0.40f}},
	{"ground_01",    "02",  1.4f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.50f, 0.35f, 0.20f}},
	{"breakstone",   "03",  2.2f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.62f, 0.60f, 0.66f}},
	{"ground_crack_01", "04", 1.1f, 6.0f, {1.00f, 0.35f, 0.08f}, {0.35f, 0.12f, 0.06f}},
	{"ice_crack",    "05",  1.8f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.61f, 0.65f, 0.68f},
			{0.55f, 0.65f, 0.70f}, 1.31f},
	{"ice",          "06",  1.8f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.61f, 0.65f, 0.68f},
			{0.80f, 0.90f, 0.95f}, 1.31f},
	// Tree trunks and branches, written by shaders/stages/trees.field.glslh. Harder than
	// ground, softer than rock: a trunk is meant to be choppable in a few swings.
	{"bark",         "07",  1.6f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.29f, 0.20f, 0.14f}},
	// Water (docs/superpowers/specs/2026-10-06-water-voxels-design.md §2). flat_albedo doubles
	// as the SCATTER colour, the body colour deep water fades to; it is also what water looks
	// like with transparency off. Red dies first: ~0.25 left after 2 m, blue ~0.77.
	{"water",        "08",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.03f, 0.16f, 0.20f},
			{0.50f, 0.82f, 0.88f}, 1.33f, Liquid::water},
	// Snow (docs/superpowers/specs/2026-10-07-fjords-terrain-design.md §7.4), banded onto
	// high, not-too-steep ground by fjord_bands. Soft. The brightest albedo in the game: if
	// sunlit snow blooms, tune flat_albedo and the texture, never the beauty stack.
	{"snow",         "09",  1.0f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.86f, 0.88f, 0.92f}},
	// Under each conifer crown (shaders/stages/conifers.field.glslh): needle litter up close,
	// and -- through its top mip -- the canopy colour of a forest seen from kilometres away.
	// Built by tools/convert_forest.sh. flat_albedo is that texture's measured mean.
	{"forest",       "10",  1.2f,    0.0f, {0.0f, 0.0f, 0.0f},   {0.11f, 0.16f, 0.04f}},
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
	{"leaf_clump",   0.0f, {0.0f, 0.0f, 0.0f}},
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

// Transparency lookups, failing soft like the rest: air, foliage and any id with no row are
// opaque (transmit 0, ior 1). Mirrored in GLSL as mat_transparent / mat_transmit / mat_ior.
bool material_transparent(uint16_t id);
void material_transmit(uint16_t id, float out[3]);
float material_ior(uint16_t id);

// Fails soft like the rest: air, foliage and any id with no row are not liquids. Mirrored in
// GLSL as mat_liquid.
Liquid material_liquid(uint16_t id);

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
