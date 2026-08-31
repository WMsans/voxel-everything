# Voxel Everything — Material System Design

**Date:** 2026-08-23
**Status:** Approved design, pre-implementation
**Scope:** Promote materials from three hardcoded lists into one authoritative table
carrying hardness and glow; apply glow in rendering and hardness in editing; add a demo
scene for choosing the material being placed.

---

## 1. Problem

Materials already half-exist, and the half that exists is scattered.

`MaterialAtlas` (`extension/src/render/material_atlas.cpp`) loads four materials × five maps
into two 512² RGBA8 texture arrays with ten mips and sixteen reserved layers.
`shaders/common.glslh` samples them triplanar with explicit gradients —
`material_surface()` for albedo, `material_props()` for roughness and AO — and the
raymarcher, the LoD fragment shader, the deferred pass and the composite pass all consume
that. Roughness drives `cel_shade`'s gloss; AO is folded into albedo by both the near-field
(`raymarch.comp.glsl`) and far-field (`lod.frag.glsl`) writers.

What is missing is everything that makes it a *system*:

1. **No registry.** A material's identity is spread across three lists that can silently
   disagree: `kMaterialNames` (`material_atlas.cpp:24`), `flat_material_albedo()`
   (`common.glslh:47`), and `MATERIALS=(...)` in `tools/convert_materials.sh`. Nothing
   checks that they agree, and there is nowhere to put a property that is not a texture.
2. **No hardness.** `generator/edit_ops.cpp` carves the same sphere through grass and
   granite. Nothing in the edit path knows a material has properties.
3. **No glow.** There is no emissive term anywhere in the engine.
4. **No way to choose.** `demo/edit_tool.gd` hardcodes `fill_material = 4` and
   `paint_material = 1` as exported defaults.

The decomposition design (2026-08-22, §3) named "material definition" as a feature the
subsystem split was meant to serve. This is that feature.

## 2. Decided constraints

| Decision | Choice |
|---|---|
| Source of truth | `constexpr` C++ table; rebuild to add a material |
| Hardness semantics | Per-point, inside the field evaluator (not tool-side) |
| Field distortion | Fixed properly via a gated Eikonal clamp, not accepted or capped |
| Glow authoring | Per-material scalar × optional per-texel mask |
| Picker form | Modal grid panel, toggled by a key |
| Existing edit tools | Unchanged — keys 1–4 and the wheel keep their current meanings |

## 3. The registry

`extension/src/world/material_table.h`:

```cpp
struct MaterialDef {
    const char *name;        // "magma"  — the picker's label
    const char *asset;       // "04"     — assets/materials/04_basecolor.png, ...
    float hardness;          // >= 1.0; see section 5
    float glow;              // emissive strength; 0.0 means not emissive
    float glow_rgb[3];
    float flat_albedo[3];    // far-field and unknown-layer fallback
};
constexpr MaterialDef kMaterials[] = { ... };
```

Material id `i + 1` is served by atlas layer `i`; material 0 is air and has no layer. That
mapping is unchanged, so no existing brick, palette, override or stored volume is affected.

Three consumers read the one table:

- **C++** includes the header. `MaterialAtlas::initialize` replaces `kMaterialNames` with
  `kMaterials[i].asset`.
- **GLSL** reads `shaders/material_table.glslh`, a committed generated file declaring
  `MAT_HARDNESS[]`, `MAT_GLOW[]`, `MAT_GLOW_RGB[]` and `MAT_FLAT_ALBEDO[]`, pulled in by
  `common.glslh`. No shader gains a binding and no descriptor set changes.
  `flat_material_albedo()` becomes a lookup into it.

  `ve::material_table_glsl()` emits that text from `kMaterials`, and a unit test asserts the
  committed file matches it byte for byte, printing the correct contents on failure. This is
  the same hand-mirror-plus-differential-test pattern `shade.glslh` and `field.glslh` already
  use, and it is chosen over registering the source through `load_shader_source`'s override
  map for two reasons: `gen_pass_->initialize` compiles `brick_gen.comp.glsl` at
  `render/orchestrator.cpp:182`, before `materials_->initialize` at `:184`, so an override
  would have to be installed ahead of the whole pass-init sequence; and
  `clear_shader_source_overrides()` — which tests call — would then leave every shader unable
  to resolve the include.
- **GDScript** calls `VoxelWorld.material_table() -> Array[Dictionary]`, one dictionary per
  material with `id`, `name`, `asset`, `hardness`, `glow`, `glow_color`, `albedo`.

`tools/convert_materials.sh` keeps its own `MATERIALS` array — it is a separate process that
cannot include a C++ header — but a unit test asserts the two lists agree, which is exactly
the drift that is currently unguarded.

## 4. Glow

`emissive = glow_rgb × glow × mask`, added in `shaders/deferred.comp.glsl` after
`cel_shade`. Lighting happens exactly once in this engine and this keeps it that way.

**The mask** is the albedo array's alpha channel. That channel currently holds the height
map, and no shader reads it — `surf.a` appears nowhere in the codebase. `pack_layer()`
writes `NN_glow.png` there instead, and packs a flat `1.0` for materials that ship no glow
PNG. The consequences of that fallback are deliberate:

- strength only, no mask ⇒ uniform glow across the surface
- mask only, no strength ⇒ nothing, because strength defaults to `0.0`
- both ⇒ textured glow

No new memory, no new bandwidth, no new binding: the channel is already allocated, packed
and mipped.

The deferred pass already binds both material arrays (bindings 8 and 9) and reconstructs
world position from depth, so it samples the mask itself:

```glsl
float g = MAT_GLOW[mat];
if (g > 0.0) {
    vec4 s = material_surface(mat, wpos, n, ddx, ddy);
    lit += MAT_GLOW_RGB[mat] * g * s.a;
}
```

Triplanar gradients come from reconstructing the world position of the neighbouring pixels
(two extra depth fetches). The entire block is skipped for any material whose strength is
zero, which is every material except magma — so non-emissive terrain pays one uniform-array
read per pixel and nothing else.

Near field and far field both get glow with no per-path work, because the deferred pass
shades both.

**SSGI needs no change.** Its `history` tap is the downsampled previous lit buffer, so
emission bounces into indirect light on the following frame automatically.

**Godot glow needs no engine change.** `inject` writes the rgba16f lit buffer into Godot's
colour texture before glow and tonemap, so emissive above 1.0 blooms natively. The only edit
is enabling glow on the `Environment` in `demo/main.tscn`, which today sets only background
and ambient.

## 5. Hardness

`ve::apply_op` divides a sphere-subtract's radius by the hardness of the material at the
sample point, and `shaders/field.glslh` mirrors it exactly:

```cpp
const float r = op.radius / MAT_HARDNESS[s.material];
if (-(dist - r) > s.sdf) { ... }
```

Because `eval_field` is the one field every consumer walks, this reaches brick generation,
raycasting, meshing, occupancy and island extraction consistently, on both CPU and GPU.
`tests/test_field_diff.gd` already diffs the two evaluators and is the gate on the mirror.

### 5.1 The hardness >= 1.0 invariant

**Hardness is clamped to `[1.0, ∞)` and static_asserted in the table.** This is load-bearing.

`op_world_aabb()` reports `pos ± op.radius`, and every region range, brick residency test,
connectivity re-mark and op-filter decision is built on it. A hardness below 1.0 would make
the carve reach *past* its own AABB, and an op that reaches outside its declared bounds is
silently dropped at region boundaries — a deleted edit, not a visual artifact. So
`op.radius` is the op's maximum reach and hardness only ever shrinks it. Softness is
expressed by authoring a larger tool radius, never by a hardness below one.

### 5.2 The field distortion, and the fix

Per-point hardness keeps the field's **sign** exactly correct — a point is air iff it lies
inside the crater its own material would take — so meshing, occupancy, collision and
residency classification are unaffected.

What it breaks is the **magnitude**. At a rock/dirt seam inside a crater, the field reports
the distance to the dirt crater's wall while the barely-carved rock lip stands much closer.
The near-field marcher sphere-traces with `t += max(d * 0.9, 0.005)` and steps through it.
Concretely: a 3 m carve against rock at hardness 3 can report roughly 1 m of clearance with
a lip 5 cm away — eighteen voxels of overshoot, seen as sparkling holes along seams.

The fix is a **Chebyshev/Eikonal clamp on the baked 17³ brick lattice**: clamp each sample's
magnitude against its six neighbours plus one voxel pitch, sweeping until it converges. That
restores the 1-Lipschitz property sphere tracing depends on, and it also tightens the
ordinary CSG overestimates the engine already carries from `max(a, -b)`.

It is **gated so the common case pays nothing**: it runs only for a brick whose palette
holds more than one material *and* whose op list contains a sphere-subtract. Both facts are
already computed during the bake, and neither holds for unedited terrain.

The clamp must exist twice — in the CPU bake and as a shared-memory sweep in
`shaders/brick_gen.comp.glsl` — because `test_field_diff.gd` and `test_brick_diff.gd` diff
the two lattices. Keeping that mirror bit-exact is the largest single piece of work in the
plan, and it is scheduled before hardness is switched on so the artifact never lands.

## 6. Demo picker

`demo/material_picker.tscn` and `demo/material_picker.gd`, built the way
`demo/settings_menu.tscn` is: scrim, centred `PanelContainer`, `PROCESS_MODE_ALWAYS`,
releasing mouse capture on open and restoring the previous mode on close. Instanced into
`main.tscn`'s HUD, toggled with **M**.

Entries are built from `material_table()`, each showing a swatch, the name, and the hardness
and glow values — the numbers are shown because displaying the properties is what
demonstrates a material system rather than merely consuming one. Selecting an entry writes
both `fill_material` and `paint_material` on `EditTool`.

Swatches load through `Image.load_from_file(ProjectSettings.globalize_path(...))` and
`ImageTexture.create_from_image()`, downscaled for display. This reads the PNG off disk at
runtime, which sidesteps the `.gdignore` on `assets/materials/` — removing that file would
pull twenty-odd textures into Godot's import pipeline for no other benefit.

The existing tools are untouched: keys 1–4 still select Carve/Fill/Paint/Drill and the wheel
still adjusts radius. The picker chooses *what* is placed, not *how*.

## 7. Content

Material `04` is **magma**, sourced from `ground_crack_01` in the local
`terrain_textures_vol2` library, which ships the same five maps the converter already
consumes. Its glow mask is generated in `tools/convert_materials.sh` as the inverted and
levelled height map, so the deep cracks glow and the raised stone stays dark. Low hardness,
hot orange glow, high strength — chosen so SSGI bounce and Godot's bloom both have something
unmistakable to react to.

## 8. Testing

**C++ (`extension/tests/`)**
- `test_material_table.cpp`: hardness >= 1.0 for every entry, unique names, layer count
  within `kMaterialLayers`, glow strengths non-negative, and agreement with the
  `MATERIALS` array in `tools/convert_materials.sh`.
- `test_material_glslh.cpp`: the committed `shaders/material_table.glslh` matches
  `ve::material_table_glsl()` byte for byte, and the failure message prints the correct
  file contents.
- `test_edit_ops.cpp` (extended): an identical carve removes strictly less volume from a
  hard material than from a soft one, and a hardness of 1.0 reproduces today's result
  exactly.
- `test_brick_eval.cpp` (extended): after the clamp, every adjacent lattice pair satisfies
  `|d[i+1] - d[i]| <= voxel + epsilon`; and the clamp does not run for a single-material
  brick.

**GDScript (`tests/`)**
- `test_field_diff.gd` (extended): the scenario gains a carve straddling two materials of
  different hardness, so the CPU/GLSL mirror of both the hardness term and the clamp is
  gated.
- `test_material_atlas.gd` (extended): the albedo array's alpha carries a non-flat mask for
  magma and flat 1.0 for a material with no glow PNG; and a magma pixel's lit value exceeds
  its albedo while a grass pixel's does not.
- `test_material_seam.gd` (new): rays marched along a hard/soft seam inside a crater agree
  with the analytic raycast — the regression test for the artifact section 5.2 exists to
  prevent.
- `test_material_picker.gd` (new): the scene opens and closes, restores the previous mouse
  mode, and selecting an entry sets both `fill_material` and `paint_material` on the tool.

## 9. Deliberate decisions

- **Rebuild to add a material, not a data file.** The texture arrays are already
  rebuild-coupled — a new material means new PNGs and a converter run — so a JSON file would
  buy runtime editability for the properties while the art stayed static, at the cost of a
  parser and a set of malformed-input failure modes on the engine's init path.
- **Height sacrificed for glow.** The height map is loaded, packed, mipped and never read.
  Repurposing its channel is free; adding a third texture array would cost roughly 22 MB and
  a new binding in four shaders so that two materials could glow.
- **Glow sampled in deferred, not carried in the G-buffer.** No G-buffer channel is free,
  and widening it or stealing precision from the material id would make every pixel pay for
  a feature that one material uses. Re-sampling costs nothing when the strength is zero.
- **Hardness in the field, not in the tool.** Scaling the radius tool-side would have been
  far cheaper and needed no mirror, but a single blast would then carve grass and granite
  identically based on whatever sat under the reticle.
- **The clamp is gated, not global.** Running an Eikonal sweep on every brick would make
  streaming pay for a correction that only edited, multi-material bricks require.
