# Geometric Normal Quantization Artifact — Design

**Date:** 2026-08-20
**Status:** Approved design, pre-implementation
**Scope:** Remove the curled, normal-map-like bands from every voxel-rendered surface without
applying material normal maps, doubling the near-field SDF atlas, or making the frame slower.
**Supersedes:** the attempted wider finite-difference normal change currently present in the
worktree.

---

## 1. Problem and measured cause

The visible pattern is not a material normal map and is not produced by SSGI, SSR, outlines,
contact shadows, either sun-shadow path, glossy rays, or islands. A deterministic capture with
all of those effects disabled retains the bands.

The near renderer stores its SDF in `R8_UNORM` over `[-0.64 m, +0.64 m]`. One code step is
approximately 5 mm. `calc_normal` differentiates the trilinearly reconstructed byte field and
feeds that result into both triplanar material projection and deferred cel lighting. The normal
error is small in continuous lighting, but the cel thresholds turn repeated threshold crossings
into the high-contrast nested curls seen in the screenshots.

A numerical reproduction using the project's `hills()` field, voxel pitch, SDF encoding, sun
direction, and cel thresholds reproduces the same curls. The analytic field normal produces a
clean cel boundary. Widening the finite-difference taps reduces a point-error metric but retains
the structured pattern; interpolating lattice gradients is mathematically equivalent for this
trilinear field. R16 removes the pattern, but the default 1088×544×544 atlas would gain about
322 MiB and is therefore rejected.

## 2. Goals and constraints

- Derive shading normals from geometry only. Material texture normal channels remain unused.
- Remove the structured curls on unedited terrain, primitive CSG edits, extracted islands,
  volume-add surfaces, and consolidated override terrain.
- Keep the main SDF atlas `R8_UNORM` and its dimensions unchanged.
- Add no more than 32 MiB of GPU memory at the configured maximum.
- Replace the raymarcher's six trilinear SDF probes (48 random 3D texture reads per hit); do not
  add a full-screen reconstruction pass.
- Preserve the existing SDF values, zero crossings, marching, min-max traversal, materials,
  collision, and LoD geometry.
- Keep all compact-normal allocation fail-soft. Exhaustion may reduce normal quality locally,
  but may never hide geometry, change collision, or corrupt a stored field.

## 3. Chosen approach

Normals come from the source field, not from the quantized render atlas.

The field evaluator gains a gradient-bearing result:

```text
FieldSample = { sdf, material, gradient }
```

This is not automatic differentiation through arbitrary GLSL. Each existing field primitive
has a small explicit distance-and-gradient implementation, and the ordered CSG evaluator carries
the gradient belonging to the distance branch that wins. The value-only evaluator remains for
bulk lattice generation; the gradient-bearing path runs once per visible ray hit.

Stored fields cannot recover their pre-quantization gradient from an R8 lattice. Their gradients
are therefore captured when the high-precision source is still available and encoded as an
octahedral `RG8_SNORM` normal. A shared, fixed-capacity normal pool holds only live or resident
stored-field normals. It is not indexed like the full brick atlas and never allocates one normal
for every possible atlas texel.

### Rejected alternatives

- **R16 SDF atlas:** correct but adds about 322 MiB at the default configuration.
- **Wider SDF taps:** no memory or dispatch cost, but visibly leaves residual bands and blurs
  sub-metre geometry.
- **Screen-space reconstruction:** universal, but camera-dependent, fragile at silhouettes, and
  adds another full-screen operation.
- **Extra bits packed into the material/min-max atlases:** avoids a new allocation but couples
  unrelated formats, complicates apron sampling, and increases reads in the hottest path.

## 4. Source-field gradients

### 4.1 Procedural base

`hills(x, z)` gains its closed-form partial derivatives. The terrain branch returns
`(-dh/dx, 1, -dh/dz)`. The carved base cave returns the negated sphere gradient when its branch
wins. The result is normalized only after all CSG operations have run.

The CPU mirror implements the same formulas. Shared constants remain in their current canonical
locations, and differential tests pin CPU and GPU behavior together.

### 4.2 Ordered CSG

Each shape returns both distance and gradient:

- sphere add: sphere gradient;
- sphere subtract: negated sphere gradient;
- box subtract: negated signed-box gradient, with deterministic axis selection at ties;
- paint: changes material only and leaves distance and gradient untouched;
- volume add: samples the stored volume distance and its compact normal together.

`min`/`max` chooses the gradient from the same operand that supplies the winning distance. Exact
ties use one documented comparison rule shared by CPU and GLSL so a boundary cannot flicker
between two normals.

The raymarcher resolves the region and ordered op span it already has, evaluates this gradient
path at the refined hit point, transforms a volume/island gradient when required, normalizes it,
and writes the existing oct-encoded G-buffer normal. Degenerate gradients fall back to geometric
up for terrain or the previous valid/facing normal for a stored field.

### 4.3 Fast path and cost

The common case is an unedited procedural hit. It evaluates the closed-form base gradient and
does not read the SDF atlas for a normal. Edited regions walk their ordered op list once at the
hit; cheap influence tests skip shapes that cannot win there. This replaces 48 random atlas
fetches rather than adding work beside them.

Reflection hits use the same function only when glossy SDF rays are enabled. Shadow rays do not
need a normal at each step. Material-normal channels remain ignored; the resulting geometric
normal continues to drive triplanar weights, lighting, reflections, outlines, and shadow bias.

## 5. Compact normals for stored fields

### 5.1 Representation

One stored normal is two signed bytes using the existing octahedral convention. Decoding yields a
unit local-space gradient. A normal sample count always matches its associated lattice sample
count; malformed data is rejected before upload.

`VolumeData` carries compact normals alongside SDF and material bytes. Island extraction already
writes one 32-bit word per sample while using only its low 16 bits, so it can return SDF,
material, and an oct normal without increasing the extraction output buffer or readback size.
Extraction evaluates the masked field gradient before quantization.

Resampling a volume interpolates decoded source normals, renormalizes, and re-encodes them.
Consolidation captures normals from the exact field evaluation that creates an override rather
than differentiating the resulting byte lattice afterward.

### 5.2 Shared pool and memory bound

A `StoredNormalPool` owns fixed-size pages and is shared by live island descriptors, resident
volume operands, and resident consolidated overrides. Descriptors/table entries carry a normal
page handle separate from their SDF/material slot.

The pool has an exported byte budget capped at 32 MiB by default and never grows implicitly.
Pages are allocated only while their source is render-reachable and are returned with the same
lifetime event that releases that source. The design deliberately avoids a normal texture with
one entry for every near-atlas slot.

Where the current island render atlas duplicates a volume already pinned in the world volume
pool, the render path references the pinned source slot directly. Removing that duplicate copy
offsets part of the compact-normal allocation and removes one upload per island creation.

### 5.3 Exhaustion and legacy data

Allocation failure records a counter and uses a bounded wide-gradient fallback from the stored
R8 lattice for that source. It does not reject an edit or island. The project has no implemented
disk save/load path, so this change needs no migration. A source already resident when render
resources are rebuilt receives normals from its CPU-authoritative `VolumeData` or
`OverrideBrick`; if that object has no compact-normal payload, it takes the explicit fallback
until its next extraction or consolidation regenerates one.

Pool telemetry reports capacity, live bytes, high-water bytes, allocation failures, and fallback
hits through the existing debug/statistics surface.

## 6. Data flow

```text
procedural base + ordered primitive ops ── field gradient at refined hit ──┐
                                                                          ├─ geometric normal
extraction/consolidation ── compact normal pool ── stored-field sampling ──┘
                                                                                 │
                                    triplanar material weights + G-buffer normal ┘
                                                                                 │
                                          deferred cel / outline / reflection ───┘
```

The R8 SDF continues to own traversal and surface position. The new path changes only how the
geometric normal is obtained after a hit is known.

## 7. Testing and acceptance

Implementation follows test-driven development.

### 7.1 Unit and differential tests

- CPU tests cover analytic `hills`, sphere, box, CSG branch selection, tie behavior, degenerate
  points, oct round trips, and volume-normal resampling.
- GPU differential tests compare the gradient-bearing GLSL evaluator with the CPU mirror at
  deterministic random points before and after each edit type.
- Pool tests cover allocation/release, reuse, capacity exhaustion, stale handles, malformed
  sample counts, and telemetry.
- Island and consolidation tests verify that compact normals survive extraction, rigid
  transforms, resampling, volume-add, and override publication.

### 7.2 Artifact regression

A deterministic GPU render fixture samples a broad patch of the actual `hills()` surface, not a
pair of adjacent points. It compares rendered `N·L` and cel-band classification with the analytic
reference. Acceptance requires:

- RMS `N·L` error at or below 0.001 on procedural terrain;
- fewer than 0.1% incorrect cel-band pixels away from the analytic band boundary;
- no connected repeated-band component matching the old R8 quantization pattern;
- the same checks on a primitive edit and an extracted island fixture;
- changing material normal-map channels does not change the geometric G-buffer normal.

The test must fail against the current wider-tap implementation before production code changes.

### 7.3 Performance and memory

- Default SDF-atlas allocation is byte-for-byte unchanged.
- Compact-normal GPU allocation is at most 32 MiB.
- The M7 deterministic capture records raymarch GPU time before and after the change with all
  beauty effects off. The median and p95 may not regress beyond run-to-run noise; a regression
  greater than 3% blocks completion and must be profiled.
- Primary-hit normal reconstruction performs no trilinear SDF-atlas probes.
- Full CPU, GPU, shutdown, and render test suites pass without validation errors.

## 8. Implementation boundaries

Expected changes are limited to:

- shared CPU/GLSL field-gradient evaluation;
- compact-normal data, extraction, resampling, pooling, descriptors, and bindings;
- ray-hit normal selection and removal of the failed wider-tap implementation;
- debug telemetry and focused regression/performance tests.

The task does not change SDF encoding, atlas dimensions, marching/traversal rules, material normal
mapping, cel thresholds, LoD geometry normals, collision, gameplay, or save semantics.
