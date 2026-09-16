# Voxel Everything — One Settings Store and Settings Panel (Sub-project 3) + S1/S2

**Date:** 2026-09-16
**Status:** Implemented; see docs/superpowers/plans/2026-09-16-settings-store-results.md
**Roadmap:** `docs/superpowers/specs/2026-09-13-frame-module-design.md` §9.2 and the pathway in
`docs/superpowers/plans/2026-09-13-frame-module.md` ("Sub-project 3 — One settings store").
**Scope:** roadmap milestones 1–6, suspected bugs S5 and S6, plus S1 and S2 (routed to
sub-project 5 by the roadmap; pulled forward here as minimal fixes on today's code). Extended
beyond the roadmap: the F1 beauty menu and the F7 graphics popup become one panel, and every dial
either shows (display, render, beauty, grass) is stored, described and persisted by the same C++
mechanism. One spec, one plan.

Vocabulary as in the roadmap spec (Ousterhout's deep/shallow modules).

---

## 1. Problem

### 1.1 Settings

- **Beauty knobs are `if (name == …)` chains.** `beauty_field` / `beauty_value_field` in
  `orchestrator.cpp:487-512` map names to `bool` and `float` members only. The five `int` knobs
  (`ssgi_taps`, `ssr_steps`, `contact_steps`, `ssao_steps`, `ssao_directions`) cannot be set by
  name. `debug_beauty_settings` (`hooks_render.cpp:406-436`) restates the list a third time.
- **Ranges live apart from names.** Beauty clamps are hand-written in `clamp_settings`
  (`beauty_settings.cpp`); grass has a member-pointer name table (`grass_settings_store.cpp:9-32`)
  but its ranges are in `clamp_grass_settings` (`grass_settings.cpp`). Beauty's `clamp_float`
  passes NaN through; grass's floors NaN. `static_cast<int>(NaN)` in
  `GrassSettingsStore::set_value` is undefined behaviour.
- **Render dials are loose fields.** `near_field_scale_`, `near_field_enabled_`,
  `islands_enabled_` are separate atomics on `RenderOrchestrator` and `quality_tier_` sits under
  `beauty_mutex_`; `set_effect_enabled` special-cases `islands` and `near_field` by name
  (`orchestrator.cpp:587-596`).
- **Two menus, two storage styles.** `demo/debug_menu.gd` (F1) hard-codes
  `EFFECTS`/`VALUES`/`GRASS`/`GRASS_VALUES` rows with their own ranges and reads beauty through
  `hooks().debug_beauty_settings()`; it persists nothing. `demo/settings_menu.gd` (F7) hard-codes
  `RESOLUTIONS`/`UPSCALERS`/`QUALITY_TIERS`, owns `user://graphics.cfg` persistence and the
  measured-run guard in GDScript, and its slider ranges live in `settings_menu.tscn`.
- **S5 — menu ranges contradict the C++ clamps.** Blade width caps at 0.06 while the default is
  0.08 (`grass_settings.h`), so opening the menu clamps the slider below the shipped value and any
  drag writes a thinner blade; emissive reach caps at 128 while the clamp allows 512; reach
  (120 vs 256), blade height (1.5 vs 4), wind strength (1.5 vs 4) and wind speed (3 vs 8) are
  also narrower than C++.
- **S6 — `set_quality_tier` discards every tweak.** `RenderOrchestrator::set_quality_tier`
  (`orchestrator.cpp:576-580`) replaces `beauty_` with `settings_for_tier(t)`. Consequences today:
  `demo/benchmark.gd:145-153` applies `--effects-off=` *before* parsing `--quality=`, so
  `--effects-off=ssgi --quality=2` silently re-enables SSGI; `settings_menu.gd`
  `apply_saved_config` sets the tier last, so any per-knob persistence would be erased.
- **Only tier, near-field and display dials persist**; nothing but `quality_tier` and
  `near_field_scale` is in the inspector.
- **Look constants outside every store:** `kSsaoRadius = 5.0` / `kSsaoStrength = 1.5`
  (`ssao_pass.cpp:10-11`); outline `0.35f` (`outline_pass.cpp:60`); contact shadow
  `{0.6f, 0.85f, 0.05f}` (`contact_shadow_pass.cpp:67`); `DeferredPass::kAmbient`
  (`deferred_pass.h:15`, also published as the `ve_ambient` global at `voxel_world.cpp:417`);
  sky gradient (`common.glslh:73-76`).

Change cost today (Appendix A): beauty float knob 9 files (11 with inspector); beauty int knob
not settable by name.

### 1.2 World-field probes (S1, S2)

Consolidation bakes a region's ops into `OverrideStore` bricks and then clears them
(`consolidation.cpp:157` `clear_region_through`). Any CPU field evaluation that passes the ops
but not the overrides sees the pre-edit terrain for that region.

- **S1 — collider residency probe.** `ColliderStreamer`'s `LogProbe`
  (`collider_streamer.cpp:32-42`) calls `ve::chunk_has_surface(gen, ops, n, chunk)` with neither
  volumes nor overrides, and `chunk_has_surface` (`mesh_chunk.h:65`) has no override parameter.
  A chunk filled in the air and then consolidated, or holding only a pasted volume, probes
  surface-free and gets no collider.
- **S2 — island contact probe and CPU island extract.** `LogContactProbe`
  (`island_manager.cpp:85-97`) passes volumes but not overrides to `contact_samples_field`
  (`contact_refine.h:60`), which has no override parameter; marginal-contact refinement then
  judges a consolidated carve by the uncarved terrain. The CPU `extract_island_volume`
  (`volume_set.h:83`) has no override input, while its GPU counterpart samples a field snapshot
  that includes overrides (`voxel_world.cpp:880-890`); only `debug_island_extract_diff`
  (`hooks_physics.cpp:571`, via `VoxelWorld::extract_component`) uses the CPU path.

Both probes already take `edit_mutex`, which is also the lock consolidation holds while it
mutates overrides (`consolidation.cpp:58`), so passing `store->overrides()` adds no lock.

## 2. Decided constraints

| Decision | Choice |
|---|---|
| Spec/plan cut | One spec, one plan: sub-project 3 milestones 1–6, S5, S6, S1, S2, the unified panel |
| Table mechanism | Member-pointer `SettingRow<T>` tables + generic `SettingsStore<T>` in `extension/src/settings/`, pure and native-tested. No X-macros, no codegen |
| Groups | Four stores on the one mechanism: `display`, `render`, `beauty`, `grass`. Separate structs; they share the mechanism only (project memory: keep new features out of the beauty stack) |
| Tier presets | `settings_for_tier()` stays a function (already pinned by `test_beauty_settings.cpp`); rows carry no per-tier column |
| Tier and tweaks (S6) | Sticky overrides: a store resolves `normalize(apply(base, overrides))`. A tier change rebases beauty; overrides survive until cleared |
| Settings manager | New C++ Node `VoxelSettings` (not part of `VoxelWorld`) with `world_path`, `viewport_path`, `config_path`, `manage_window` exports. It owns the `display` store, persistence (`ConfigFile`), the measured-run guard and the inspector properties, and addresses the other three stores through the world |
| Persistence | C++ in `VoxelSettings`: one `[group]` section per group holding exactly that group's overrides, in `user://settings.cfg`. A measured run never loads or saves. The legacy `user://graphics.cfg` is ignored, not migrated (developer preferences, re-set once) |
| Inspector | Dynamic `display/*`, `render/*`, `beauty/*`, `grass/*` properties on `VoxelSettings`. `VoxelWorld` gains no settings methods or properties; its existing ones stay as writes into the same stores |
| UI | One modal panel, `demo/settings_menu.{gd,tscn}`, toggled by **F1 only**; a `TabContainer` with Display, Render, Beauty, Grass tabs, every control built from `VoxelSettings.describe(group)`. F7 is freed; `demo/debug_menu.gd` is deleted |
| Stray constants | SSAO radius/strength, outline darken, contact-shadow triple, ambient move into `BeautySettings`. Sky gradient stays in GLSL (deferred, §8) |
| S1/S2 scope | Thread overrides (and volumes for S1) through the existing probes. No `WorldField`; `max_override_bricks = 1` in `test_connectivity.gd` stays (sub-project 5) |
| Bug-fix discipline | Each of S1, S2, S5, S6 gets a failing test first, then its own `fix:` commit, separate from any move |

### Decisions made during design

1. **A resolved value is a pure function of `(base, overrides)`.** Today `clamp_settings` writes
   `ssgi = false` into the live struct when `ssgi_taps` reaches 0, and it stays false after taps
   are raised again. After this work, raising taps back re-enables SSGI unless `ssgi` itself was
   overridden. Intended; any test that pins the old latch is updated in the migration commit with
   that cause.
2. **Setting a knob to its default value still records an override.** Only `clear` removes one.
   Predictable; the persisted file names exactly what was touched.
3. **NaN floors to the row's minimum; ±inf clamp to the nearer bound**, for every kind, in every
   store (grass behaviour today, extended to the rest and to int conversion).
4. **Colour and enum are kinds.** Ambient is one `kColor` row; quality tier, upscaler and
   resolution are `kEnum` rows (an index plus option labels), so the inspector and the panel show
   one control each.
5. **Soft UI ranges.** A row carries hard `min/max` (the clamp) and `ui_min/ui_max/step` (the
   slider). Invariant, native-tested: `min ≤ ui_min ≤ default ≤ ui_max ≤ max` for every row and
   every tier's preset. This makes S5 unrepresentable while letting `max_blades` (hard max
   4,000,000) have a usable slider.
6. **Rows carry an optional `hint`**, shown as the control's tooltip. The near-field explanation
   in `settings_menu.tscn`'s `Hint` label moves into the `near_field_scale` row.
7. **Render dials keep their lock-free reads.** The `render` store is the source of truth; every
   resolve mirrors `near_field_scale`, `near_field` and `islands` into today's atomics, so the
   render thread (`island_slot_count`, `frame_settings`) takes no new lock.
8. **"Shipped" is the override set at `VoxelSettings::_ready`** — whatever the scene applied
   (e.g. `main.tscn`'s `near_field_scale = 0.4`) before any saved file is loaded. Reset clears
   every group's overrides and re-applies the shipped ones. Shipped beauty has no overrides, so
   Reset also returns beauty to the pure tier.
9. **`test_deferred.gd` keeps its ambient literals.** They are probe points for the GPU-vs-CPU cel
   diff (same reasoning as sub-project 4 decision 3).

## 3. Target design

### 3.1 `extension/src/settings/settings_table.h` (header-only, no godot-cpp)

```cpp
enum class SettingKind : uint8_t { kBool, kInt, kFloat, kColor, kEnum };

struct SettingValue {        // bool/int/float/enum index in v[0]; colour in v[0..2]
	SettingKind kind;
	float v[3];
};

template <class T> struct SettingRow {
	const char *name;        // config key, property suffix, GDScript name
	const char *label;       // panel text
	SettingKind kind;
	bool T::*b = nullptr;    // exactly one member pointer is non-null, matching kind
	int T::*i = nullptr;     // kInt and kEnum
	float T::*f = nullptr;
	float (T::*c)[3] = nullptr;
	float min, max;          // hard clamp (per channel for colour); enum: [0, option count - 1]
	float ui_min, ui_max, step;
	std::span<const char *const> options = {}; // kEnum labels
	const char *hint = nullptr;
};
```

Free functions over `std::span<const SettingRow<T>>`: `find(rows, name)`,
`write(row, T*, SettingValue)` (NaN-safe clamp to `[min, max]`; ints and enums rounded after
clamping), `read(row, const T&) → SettingValue`, `clamp_all(rows, T*)`.

`clamp_settings(BeautySettings*)` and `clamp_grass_settings(GrassSettings*)` keep their names
and become `clamp_all(rows, s)` followed by the struct's `normalize`. The range lines inside them
are deleted.

### 3.2 `extension/src/settings/settings_store.h` (header-only, no godot-cpp)

```cpp
struct RowInfo {             // a SettingRow without the member pointer, for type-erased callers
	const char *name, *label, *hint;
	SettingKind kind;
	float min, max, ui_min, ui_max, step;
	std::span<const char *const> options;
};

class SettingsGroup {        // what VoxelSettings and the debug hooks see
public:
	virtual ~SettingsGroup() = default;
	virtual std::vector<RowInfo> rows() const = 0;
	virtual bool get(const char *name, SettingValue *out) const = 0;
	virtual bool get_default(const char *name, SettingValue *out) const = 0; // from base
	virtual bool set(const char *name, SettingValue v) = 0;  // false: unknown name / wrong kind
	virtual std::vector<std::pair<const char *, SettingValue>> overrides() const = 0;
	virtual bool clear(const char *name) = 0;
	virtual void clear_all() = 0;
};

template <class T> class SettingsStore : public SettingsGroup {
public:
	using Resolved = void (*)(const T &, void *ctx);  // called after every resolve, outside the lock
	SettingsStore(std::span<const SettingRow<T>> rows, void (*normalize)(T *), T base,
			Resolved on_resolved = nullptr, void *ctx = nullptr);
	void set(const T &base);                        // replace base, clear overrides
	void rebase(const T &base);                     // replace base, keep overrides
	bool set_value(const char *name, float v);      // bool/int/float/enum convenience
	float value(const char *name) const;
	T get() const;                                  // resolved snapshot
	T base() const;
	// + the SettingsGroup overrides
};
```

One mutex guards `base_`, `overrides_` and `resolved_`. Every mutation re-resolves:
`resolved_ = base_; for each override: write(row, &resolved_, v); normalize(&resolved_)`, then
calls `on_resolved(copy)` after releasing the lock. The mutex is never held by render work: the
frame keeps taking value snapshots.

`GrassSettingsStore` becomes `SettingsStore<GrassSettings>` constructed with `kGrassRows` and base
`GrassSettings{}`; `extension/tests/test_grass_settings.cpp` and `test_grass_layout.cpp` compile
and pass unchanged.

### 3.3 The four groups

| Group | Struct (file) | Store owner | Base | `on_resolved` |
|---|---|---|---|---|
| `display` | `DisplaySettings {render_scale, upscaler, resolution, fullscreen}` (`settings/display_settings.{h,cpp}`) | `VoxelSettings` | captured from the `Viewport` and, when `manage_window`, the window at `_ready` | push `scaling_3d_scale`, `scaling_3d_mode` to the Viewport; `window_set_mode` / `window_set_size` only when `manage_window` and the value differs from the live window |
| `render` | `RenderSettings {quality_tier, near_field_scale, near_field, islands}` (`settings/render_settings.{h,cpp}`) | `RenderOrchestrator` | `{kHigh, 0.66, true, true}` | mirror the three dials into today's atomics; rebase `beauty` on `settings_for_tier(quality_tier)` when the tier changed |
| `beauty` | `BeautySettings` (`shade/beauty_settings.{h,cpp}`) | `RenderOrchestrator` | `settings_for_tier(tier)` | `VoxelWorld` re-publishes `ve_ambient` (main thread) |
| `grass` | `GrassSettings` (`grass/grass_settings.{h,cpp}`) | `RenderOrchestrator` | `GrassSettings{}` | — |

- `display` rows: `render_scale` float hard `[0.25, 1]`, ui `[0.5, 1]`, step 0.01;
  `upscaler` enum Bilinear / FSR 1 / FSR 2 / MetalFX spatial / MetalFX temporal (indices map to
  `Viewport::SCALING_3D_MODE_*` in `display_settings.cpp`); `resolution` enum over the five sizes
  now in `settings_menu.gd` (`1280×720` … `3840×2160`), hard min `-1`: `-1` means "the window is
  not a preset size", is never rounded to a neighbour, and writing it changes no window. The panel
  shows `W x H` of the live window for `-1`, preserving today's `show_resolution` behaviour (the
  row invariant uses `ui_min = -1` for this row); `fullscreen` bool.
- `render` rows: `quality_tier` enum Off/Low/Medium/High; `near_field_scale` float `[0.1, 1]`,
  step 0.01, with the near-field hint; `near_field` bool; `islands` bool.
- `BeautySettings` gains `ssao_radius = 5.0f`, `ssao_strength = 1.5f`,
  `outline_darken = 0.35f` (`OutlinePush.params.z`, the edge colour multiplier),
  `contact_reach_m = 0.6f`, `contact_strength = 0.85f`, `contact_bias_m = 0.05f`
  (`ContactShadowPush.params.xyz`: march reach, apply strength, surface bias / hit thickness) and
  `ambient[3] = {0.16f, 0.19f, 0.26f}`. Tier presets do not change these, so every golden is
  unchanged by construction. `kBeautyRows` hard ranges equal today's `clamp_settings` ranges; new
  rows get ranges chosen in the plan and pinned by the invariant test.
- The passes read the new beauty fields from the snapshot they already receive: `SsaoPass`,
  `OutlinePass`, `ContactShadowPass` push them; the frame fills `DeferredPass` params' ambient from
  `beauty.ambient`. `kSsaoRadius`, `kSsaoStrength` and `DeferredPass::kAmbient` are deleted.

### 3.4 `RenderOrchestrator` and `VoxelWorld`

- The orchestrator replaces `beauty_mutex_`, `beauty_`, `quality_tier_`, the two name functions
  and the `islands`/`near_field` special cases with three stores and a
  `ve::SettingsGroup *settings_group(const char *name)` accessor (`render`, `beauty`, `grass`).
  `beauty_settings()`, `grass_settings()`, `frame_settings()`, `island_slot_count()` and the
  atomics they read keep their signatures.
- `VoxelWorld` keeps `quality_tier`, `near_field_scale`, `set/get_effect_enabled`,
  `set/get_effect_value`, `set/get_grass_value` with unchanged signatures; each becomes a
  `set`/`get` on the matching group. `set_effect_enabled` resolves `islands` and `near_field` in
  the `render` group and everything else in `beauty`; `set_effect_value` accepts int and float
  beauty rows, so `set_effect_value("ssgi_taps", 4)` works. Unknown names stay fail-soft.
- `debug_beauty_settings` builds its dictionary from the `beauty` rows plus `islands`, `tier` and
  `flags`; its keys are a superset of today's.

### 3.5 `VoxelSettings` (`extension/src/voxel_settings.{h,cpp}`, registered in `register_types.cpp`)

Node. Exports: `world_path: NodePath`, `viewport_path: NodePath` (empty = own viewport),
`config_path: String = "user://settings.cfg"`, `manage_window: bool = true`.

| Method | Behaviour |
|---|---|
| `describe(group) → Array[Dictionary]` | per row: `name`, `label`, `hint`, `kind` (`"bool"`/`"int"`/`"float"`/`"color"`/`"enum"`), `min`, `max`, `ui_min`, `ui_max`, `step`, `options`, `value`, `default` |
| `get_setting(group, name) → Variant` / `set_setting(group, name, value) → bool` | bool/int/float/`Color`/int index by kind |
| `get_overrides(group) → Dictionary` / `clear_overrides(group)` | |
| `groups() → PackedStringArray` | `["display", "render", "beauty", "grass"]`, panel tab order |
| `save()` | writes one section per group containing exactly its overrides (sections rewritten, not merged); no-op in a measured run |
| `reset_to_shipped()` | clears every group's overrides and re-applies the shipped set |
| `is_measured_run() → bool` | true when user args contain `--benchmark*` or `--capture` (moved from `settings_menu.gd`) |

`_ready`: resolve world and viewport; apply property values the scene set before `_ready`
(buffered); capture the `display` base; record the shipped override set; unless measured, load
`config_path` and apply each section's keys through `set_setting` (unknown keys ignored). The
`display` store and the group dispatch live here; `VoxelWorld` knows nothing about this class.

Inspector: `_get_property_list` emits `<group>/<name>` for every row (`PROPERTY_HINT_RANGE` from
`ui_min,ui_max,step`; `PROPERTY_HINT_ENUM` from `options`); `_property_can_revert` is true and
`_property_get_revert` returns `default`, so a scene stores only real overrides.

### 3.6 The panel (`demo/settings_menu.{gd,tscn}`)

- `settings_path: NodePath` replaces `world_path`/`viewport_path`/`config_path`; the scene keeps
  `Scrim`, `Center/Panel`, a `TabContainer`, and `Reset`/`Close` buttons.
- F1 toggles; opening frees the mouse and records the previous mouse mode; Esc or Close closes,
  restores the mouse mode and calls `settings.save()`; `_exit_tree` saves if open. Behaviour
  otherwise as F7 today.
- One tab per `settings.groups()` entry, rows from `describe(group)`: `CheckBox` (bool);
  `HSlider` + value label (int/float, `ui_min`/`ui_max`/`step`); `OptionButton` (enum);
  `ColorPickerButton` (colour); `tooltip_text = hint`. Control node names are the row names
  (`Tabs/<Group>/<name>`). Opening re-reads every control from `get_setting` (no GUI-side state).
  Slider drags do not resync mid-drag (today's rule).
- `demo/debug_menu.gd` and `main.tscn`'s `DebugMenu` node are deleted; `main.tscn` gains a
  `VoxelSettings` node after `VoxelWorld` and points the panel at it. `help.gd` lists
  `["Settings", "F1"]` and drops the F7 row. `demo/benchmark.gd` routes `--render-scale=` and
  `--upscaler=` through `VoxelSettings.set_setting("display", …)`; `--near-scale=`, `--quality=`,
  `--effects-off=`, `--effect-value=`, `--grass=` keep calling `VoxelWorld` (same stores).

## 4. Testing

### 4.1 Characterization (before any production change)

- Baseline gdUnit failure set recorded at the branch's start commit
  (`docs/superpowers/plans/2026-09-16-settings-store-baseline.md`).
- Native: every beauty name in today's two chains and every grass name in today's table
  round-trips; `settings_for_tier` output pinned field-by-field for all four tiers (extending
  `test_beauty_settings.cpp`).
- gdUnit: every beauty, grass and render dial round-trips through `VoxelWorld`'s public API;
  `test_debug_menu.gd` and `test_settings_menu.gd` stay green as the behavioural contract the
  unified panel must keep (their cases are ported, not dropped, in the panel task).
- The goldens that will guard the constant moves are proven to bite: temporarily change each
  literal (SSAO radius, outline 0.35, contact triple, ambient) and observe
  `test_ssao_golden.gd`, `test_frame_shipped_golden.gd` (outline), `test_contact_shadow_golden.gd`
  and `test_deferred_golden.gd` fail; revert. A literal no golden catches gets a golden case first.

### 4.2 Failing tests for the bugs

| Bug | Test (red on today's code) |
|---|---|
| S1 | gdUnit: add-fill an air chunk inside the physics radius, `debug_consolidate_region`, let colliders re-probe; `debug_chunk_collider_info` reports a collider. Second case: a chunk holding only a `debug_apply_volume_add` volume. Native: `chunk_has_surface` over an air generator with an `OverrideStore` holding a solid brick returns true |
| S2 | gdUnit: `debug_island_extract_diff` over a carve that has been consolidated reports CPU ≡ GPU. gdUnit: a pillar whose neck is carved, then consolidated, detaches on the next connectivity run as it does unconsolidated. If the anchoring case is not red on today's code, that half is closed with the evidence in the results report |
| S5 | gdUnit: for every slider in `debug_menu.gd`, the world's shipped value lies inside the slider range, and the slider range lies inside the C++ clamp. The clamp is discovered on a separate throwaway world by writing ±1e9 and reading back |
| S6 | gdUnit: `set_effect_enabled("ssr", false)`, `set_effect_value("ssgi_strength", 2)`, then `quality_tier = 2`; both tweaks survive. `benchmark.gd`-order case: effects-off then tier |

### 4.3 New tests with the mechanism

- Native `test_settings_table.cpp`: write/read per kind, NaN floors and ±inf clamps, int and enum
  rounding, unknown name, kind mismatch; store rebase keeps overrides, `set(T)` clears them,
  `clear`/`clear_all`, resolved value independent of override order, `on_resolved` called outside
  the lock; row invariant `min ≤ ui_min ≤ default ≤ ui_max ≤ max` for every row of all four
  tables and every tier preset; enum `max == options.size() - 1`; every row name unique and no two
  rows name the same member.
- Native: `render` store tier change rebases a beauty store and keeps its overrides.
- gdUnit `test_voxel_settings.gd` (SubViewport, throwaway `config_path`, `manage_window = false`):
  `describe` matches rows for every group; overrides round-trip through `save()` and a fresh
  node's load; a measured run neither loads nor saves; `reset_to_shipped` restores the scene's
  overrides; `display/render_scale` reaches the SubViewport; inspector property
  `beauty/ssgi_strength` set/get/revert; property values set before `_ready` apply; ambient change
  reaches `ve_ambient`.
- gdUnit `test_settings_menu.gd` (rewritten): F1 toggles and Esc dismisses; closing saves; every
  tab has a control per row; each control writes only its named row; quality selection rebases
  beauty but keeps overrides; opening resyncs from the settings node; ported cases from
  `test_debug_menu.gd` (islands is a real render effect, clamping on the way in, grass density
  writes only its knob) and from the old `test_settings_menu.gd` (near-field toggle is a real
  render effect, resolution table ordered and offering the project default, off-table window size
  shown rather than blank).

### 4.4 Regression rules

Same as the frame-module plan: compare gdUnit results against the baseline by case name and
message; a suite's case count dropping is a failure; GPU timing values are never pinned; a
constant move that changes a golden stops the task. A test deleted with `debug_menu.gd` must name
its ported replacement in the commit message.

## 5. Task order

1. Baseline failure set.
2. Characterization (§4.1).
3. S1: failing tests → `fix:` commit.
4. S2: failing tests → `fix:` commit.
5. S5: failing test → `fix:` commit on today's `debug_menu.gd` ranges.
6. S6: failing test committed as expected-red (marked in the test with the task that turns it
   green).
7. `settings/settings_table.h` + `settings_store.h` with native tests.
8. Grass on the store; grass tests unchanged.
9. Beauty on the store preserving today's discard-on-tier behaviour (`set_quality_tier` calls
   `set(T)`); int knobs settable by name; beauty `if (name ==` chains deleted.
10. `fix:` S6 — the tier rebases; the S6 test goes green.
11. Stray constants, one commit each: SSAO → outline → contact shadow → ambient (with
    `ve_ambient`). Each commit: its golden unchanged.
12. `render` group: `RenderSettings` store in the orchestrator, atomics mirrored, `islands` /
    `near_field` special cases deleted, `debug_beauty_settings` from rows.
13. `VoxelSettings` node: `display` group, group dispatch, `describe`, persistence, measured-run
    guard, `test_voxel_settings.gd`.
14. Unified F1 panel: `settings_menu.{gd,tscn}` rebuilt on `VoxelSettings`; `debug_menu.gd`,
    `DebugMenu` node and F7 deleted; `main.tscn`, `help.gd`, `benchmark.gd` updated; tests ported.
15. Inspector properties on `VoxelSettings`.
16. Results report, Appendix A retrace, roadmap and §10 status rows.

## 6. Exit criteria

- `rg 'if \(name == "' extension/src/render/orchestrator.cpp` returns nothing.
- `rg 'kSsaoRadius|kSsaoStrength|kAmbient|beauty_value_field|beauty_field' extension/src` returns
  nothing.
- `demo/debug_menu.gd` does not exist; `rg 'ConfigFile|KEY_F7|RESOLUTIONS|UPSCALERS|QUALITY_TIERS|GRASS_VALUES' demo`
  returns nothing; `rg 'min_value|max_value' demo/settings_menu.tscn` returns nothing.
- Appendix A "beauty float knob" retraced at ≤ 4 files (struct field + row, pass push, shader;
  panel, inspector, persistence and debug dictionary follow automatically); "beauty int knob"
  settable by name; a new display or render dial costs its struct field + row + its apply line.
- S1, S2, S5, S6 tests pass; S1, S2, S5, S6 rows in roadmap §10 marked FIXED with commits (or the
  S2 anchoring half CLOSED with evidence).
- `test_frame_shipped_golden.gd` and every pass golden unchanged; no gdUnit failure outside the
  baseline.
- The four groups remain separate structs and stores; `VoxelWorld` has no persistence or
  describe code.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Sticky overrides surprise a test or script that expects a tier to reset knobs | Characterization finds them; `clear_overrides` / Reset are the explicit resets; the `benchmark.gd` behaviour change is intended and named in the S6 commit |
| A measured run picks up saved or inspector-stored knobs | Load and save are guarded in C++; `main.tscn` is checked after task 15 to store no `VoxelSettings` property unless it differs from default; the benchmark prints the override set of every group |
| `display` store goes stale when something writes the Viewport directly | Only `VoxelSettings` writes `scaling_3d_*` in `demo/` after task 14 (`rg scaling_3d_ demo` shows only reads); `benchmark.gd` routes through it |
| Window changes during tests | `manage_window = false` in every test; the store still describes the rows |
| `on_resolved` re-entering a store (tier → beauty rebase) deadlocks | Callbacks run after the owning store's lock is released; the render→beauty edge is one-way; native test covers it |
| Render thread takes a new lock | Render reads stay on the mirrored atomics and existing snapshots; `rg` in the plan checks `frame.cpp` gains no store call |
| `ve_ambient` and deferred ambient diverge | One source (`beauty.ambient`), published from `on_resolved`; gdUnit reads the global after a change |
| Adding overrides to `extract_island_volume` changes CPU/GPU diff results for unconsolidated regions | Overrides are empty there, so `eval_field` takes the same path; `test_island_extract.gd` must stay green |

## 8. Out of scope

- Sky gradient knobs. Trigger: the next sky or atmosphere change; first milestone is a shared sky
  block read by the four `sky_color` callers.
- Migrating `user://graphics.cfg`.
- `WorldField`, `EditPipeline`, S3 and removing `max_override_bricks = 1` (sub-project 5).
- S8 timing labels (sub-project 2, unchanged).
- A `BeautyProfile` Resource or per-scene profiles.

## 9. Amendments from planning

1. **S1 test uses a chunk that is not resident when consolidation lands.** `ChunkResidency::update` never re-probes a resident chunk (`chunk_residency.cpp:181`), so a filled chunk that already has a collider keeps it. The bug shows when the chunk is first probed *after* the bake.
2. **S2 anchoring half is tested at the probe the refinement calls**, not end-to-end. `IslandManager` gains `contact_samples(cell, axis)`; `LogContactProbe` forwards to it and a new hook `debug_contact_samples` calls the same member. An end-to-end detach test would depend on flood windows and extraction timing that are not what S2 is about.
3. **The S6 failing test is written in Task 9, immediately before its fix**, rather than committed red in an earlier task (gdUnit has no expected-failure marker, and a red case would pollute every comparison in between).
4. **`ve_ambient` needs no listener.** `VoxelWorld::update_sun_state()` already runs every `_process` on the main thread and publishes `ve_ambient`; it reads `beauty_settings().ambient` instead of `DeferredPass::kAmbient`. Only the `render` store has a listener.
5. **`debug_beauty_settings` is built from the rows in Task 8**, together with the beauty store, using a shared `setting_to_variant` helper (`extension/src/settings/godot/setting_variant.{h,cpp}`).
6. **`VoxelSettings` uses stand-in stores instead of a pending buffer.** Before `_ready` resolves the world (and always in the editor) `render`/`beauty`/`grass` resolve to node-owned stand-in stores; `_ready` copies their overrides into the world's stores.
7. **The guard is testable:** `apply_config(args: PackedStringArray) -> bool` is bound and `_ready` calls it with `OS.get_cmdline_user_args()`. `save()` refuses after a measured `apply_config`.
8. **The panel addresses controls through `control(group, name)`**, not node paths.
9. **Picking a resolution while fullscreen changes nothing until fullscreen is off** (today's F7 left fullscreen first). `# ponytail:` comment in `apply_display`.
10. **Resolution table ordering and label checks are native** (`test_display_settings.cpp`); the panel test keeps "offers the project default" and "off-table size shown".
