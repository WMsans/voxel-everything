# Task 14 — exit evidence, results report and roadmap status

## Status

DONE_WITH_CONCERNS. The edit-pipeline exit scans, native build/tests, documentation updates, and required results report are complete. The full GPU run retains the documented ambient Metal error and an unresolved sun-cascade failure; GPU/environment drift is likely, and no passing data was invented.

## Step 1 — full regression

Command, exactly as required:

```bash
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
cd extension && scons -Q test; cd ..
./gdunit_tests.sh
```

Build output:

```text
==> Build OK: 3.2M libvoxel_everything.macos.template_debug.universal.dylib
    Registered native classes: VoxelWorld, RaymarchCompositor
==> Done.
```

Native output:

```text
[doctest] test cases:     645 |     645 passed | 0 failed | 0 skipped
[doctest] assertions: 9120150 | 9120150 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Full gdUnit output summary:

```text
Overall Summary: 509 test cases | 1 errors | 1 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (96/96)
Executed test cases : (509/509)
Open XML Report at: file:///Users/jeremyzhao/Development/godot/voxel-everything/.worktrees/edit-pipeline/reports/report_46/results.xml
Exit code: 100
```

Task 1 Step 3's extraction script was run on `reports/report_46/results.xml`. Its failure/error output was:

```text
test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
test_sun_cascades_gpu::test_sub_texel_motion_rebuilds_no_cascade — FAILED: res://tests/test_sun_cascades_gpu.gd:73
```

The baseline file records:

```text
# 95 suites; declared per-suite total 507 tests
# test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
# test_sun_cascades_gpu::test_the_min_level_clamp_does_not_peter_pan — FAILED: res://tests/test_sun_cascades_gpu.gd:101
```

The comparison found these declared suite-count differences:

```text
baseline markdown suites=95; current XML suites=96
current totals: 513 tests, 1 failures, 1 errors
suite count differences:
  test_consolidation: baseline=(18, 0, 0) current=(20, 0, 0)
  test_edit_fanout: baseline=None current=(4, 0, 0)
  test_sun_cascades_gpu: baseline=(7, 2, 0) current=(7, 1, 0)
new suites: ['test_edit_fanout']
removed suites: []
```

The connectivity count is 33 in both baseline and current. Task 3 added the staleness pin, Task 10 renamed it and added the consolidated-away append regression, and Task 9 deleted `test_post_spawn_carve_rejection_keeps_body_in_hole` and `test_near_cap_carve_is_refused_before_any_carve`; therefore the net count is zero. The hold seam is a fixture change, not a case. `test_consolidation` adds `test_a_full_op_list_does_not_stay_full_while_consolidation_runs` and `test_a_held_consolidation_keeps_a_full_op_list_full`. `test_repro_pillar_debris` remains one case and its landing dump loses `land_carve_nothing` and `land_carve_restored`.

The unchanged ambient error is the documented Godot/Metal `Nil` error. The sun failure case/message changed, so the required control was run before attribution.

## Sun failure control and reruns

The working tree had no changes to stash before the control. Parent-control command sequence:

```text
git checkout HEAD^
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
./gdunit_tests.sh -a res://tests/test_sun_cascades_gpu.gd
git checkout feat/edit-pipeline
```

Parent commit: `a725648`. Parent build: `Build OK: 3.2M libvoxel_everything.macos.template_debug.universal.dylib`. Parent focused output:

```text
Statistics: 4 test cases | 0 errors | 1 failures | 0 flaky | 0 skipped | 0 orphans | PASSED
Overall Summary: 4 test cases | 0 errors | 1 failures | 0 flaky | 0 skipped | 0 orphans |
Open XML Report at: file:///Users/jeremyzhao/Development/godot/voxel-everything/.worktrees/edit-pipeline/reports/report_47/results.xml
Exit code: 100
```

The parent passed `test_sub_texel_motion_rebuilds_no_cascade` and failed `test_the_min_level_clamp_does_not_peter_pan` with `Expecting: 'true' but is 'false'` at line 108. Because the parent and current runs failed different cases, the relationship to the edit-pipeline changes remains unresolved; GPU/environment drift is likely.

Current-commit focused rerun:

```text
./gdunit_tests.sh -a res://tests/test_sun_cascades_gpu.gd
```

It failed earlier during shader compilation (`island_cull.comp.glsl` and `mesh_field.comp.glsl`, `syntax error, unexpected LEFT_OP`) with:

```text
Overall Summary: 1 test cases | 0 errors | 8 failures | 0 flaky | 0 skipped | 0 orphans |
Open XML Report at: file:///Users/jeremyzhao/Development/godot/voxel-everything/.worktrees/edit-pipeline/reports/report_48/results.xml
Exit code: 100
```

No passing result is claimed from this flaky rerun. The durable results report uses the complete `report_46` run.

## Step 2 — exit scans

Exact commands and real output:

```text
$ rg 'WorldStore::append_edit\(|append_edit_locked|struct EditSink|struct ConsolidationSink|on_edit_appended|LodSystem::note_edit' extension/src
# empty; exit 1

$ rg -il 'lock order' extension/src
extension/src/core/edit_pipeline.h
# exit 0

$ rg 'max_override_bricks' tests/test_connectivity.gd
# empty; exit 1

$ rg 'collect_ops_for_aabb|edit_log\(\)->ops\(' extension/src/physics
# empty; exit 1
```

One stale deleted-symbol mention in a comment in `extension/src/lod/lod_system.h` was removed so the first required scan is actually empty; this did not alter behavior.

## Step 3 — sink-body audit

Command:

```bash
rg -n 'void .*::record\(const ve::Invalidation' extension/src
```

Output:

```text
extension/src/voxel_world.cpp:849:void VoxelWorld::record(const ve::Invalidation &inv) {
extension/src/physics/island_manager.cpp:154:void IslandManager::record(const ve::Invalidation &inv) {
extension/src/core/world_store.cpp:31:void WorldStore::record(const ve::Invalidation &inv) {
extension/src/mesh/consolidation.cpp:20:void ConsolidationCoordinator::record(const ve::Invalidation &inv) {
extension/src/lod/lod_system.cpp:368:void LodSystem::record(const ve::Invalidation &inv) {
```

Each body was read:

- `VoxelWorld::record`, line 849: accepted edit/consolidation ranges are merged into `pending_dirty_`; rejected edits increment `edit_rejections` and log. No mutex.
- `IslandManager::record`, line 154: copies accepted `{EditOp, seq}` into `inbox_`. No mutex.
- `WorldStore::record`, line 31: copies `{EditOp, AppendResult}` into `pending_edits_`. No mutex.
- `ConsolidationCoordinator::record`, line 20: queues regeneration or consolidation regions. No mutex.
- `LodSystem::record`, line 368: merges boxes into `pending_marks_`. No mutex.

The only non-queue work is the VoxelWorld rejection-stat/logging half, as allowed by the rule. No `record()` body takes a mutex.

## Step 4 — change-cost trace

Spec §8 before/after:

```text
A new consumer that must hear about edits: before 5 files; after 2 files.
After files: the consumer's .h and .cpp, plus the existing one registration site.
```

## Step 5 — results report

Created `docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`. It records the complete regression summary, baseline deltas, unchanged pins, removed lock edges, sink rule, exit checks, change cost, deletion list, `debug_mesh_submit` open item, and deferred-minor triage.

## Step 6 — spec and roadmap

- `docs/superpowers/specs/2026-09-17-edit-pipeline-design.md`: status is now `Implemented; see docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`; appended `## 11. Decided during planning` with all fourteen numbered decisions verbatim from the implementation plan.
- `docs/superpowers/plans/2026-09-13-frame-module.md`: replaced only the Sub-project 5 status paragraph with the required 5a/5b implemented text.

Validation output:

```text
status/decision validation: PASS (14 numbered decisions verbatim; roadmap status exact)
```

## Step 7 — commit

Completed in `b806a3c` (`docs: record edit pipeline exit criteria and results`). Validation status: the required documentation commit is present; the accidental comment-only source hunk was reverted in the round 1 fix without changing the Task 11 implementation.

## Concerns

- Full gdUnit is not clean: the ambient Metal/Godot error remains, and the sun-cascade suite is GPU-flaky as demonstrated by the parent control and current rerun.
- The task report's durable baseline comparison uses the committed baseline markdown because the historical `reports/report_2` XML directory is no longer present.
- Deferred ledger items remain historical documentation/evidence caveats except Task 6's registration/removal concurrency, which has review evidence but no direct test, and Task 9's store-before-preflight comment mismatch.

## Round 1 fix report

- Reverted only the Task 14 comment hunk in `extension/src/lod/lod_system.h`; no production behavior or Task 11 implementation changed.
- Reworded the sun-cascade finding in the results report and this report: the current failure remains unresolved, with GPU/environment drift likely rather than confirmed unrelatedness.
- Replaced the results report's placeholder documentation commit with `b806a3c`.
- Marked Step 7 completed with commit `b806a3c` and its validation status above.

Validation:

```text
`git diff --check` — PASS
report consistency check — PASS
```

## Final fix wave

### Status

DONE_WITH_CONCERNS. The final review wave fixed the remaining island-extraction staleness hole,
removed the last deleted-symbol text from `extension/src/lod/lod_system.h`, and refreshed the
results/report evidence. No subagents were dispatched.

### Finding and root cause

`IslandManager::land_extraction()` previously used `last_seq() > f.log_seq` only when the
post-snapshot spatial query was empty. A relevant append could be consolidated away while an
unrelated later op remained in the queried AABB; the nonempty query then skipped the global
sequence check, and no retained op reached the island boxes. The extraction was incorrectly
landed against an obsolete snapshot.

### Fixes

- `extension/src/physics/island_manager.cpp`: use
  `const bool stale = handles_.store->edit_log()->last_seq() > f.log_seq;` for every successful
  extraction landing. Equal-sequence consolidation remains not stale.
- `tests/test_connectivity.gd`: strengthen the real regression to use a cross-region component,
  consolidate the relevant post-snapshot edit away, retain an unrelated later paint op inside the
  extraction AABB, and require `land_stale` to increment.
- `extension/src/lod/lod_system.h`: reword the comment that still named the deleted append path.
- `.superpowers/sdd/2026-09-17-edit-pipeline/task-10-report.md` and
  `docs/superpowers/plans/2026-09-17-edit-pipeline-results.md`: record the final fix and fresh
  evidence.

Source/test fix commit: `ca30d9f fix: conservatively reject any newer island extraction`.
The documentation update containing this report is the subsequent documentation commit on this
branch.

### TDD evidence

The regression was edited first, then run against the existing binary before the production
rebuild:

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd
```

`reports/report_50/results.xml` recorded 33 cases, 1 failure, and 0 errors. The new case failed
with `land_stale == 0` and a spawned body after the relevant append was consolidated away while a
later retained op remained, which is the expected RED failure.

After the production change and rebuild:

```text
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

Build passed and linked `libvoxel_everything.macos.template_debug.universal.dylib`.

```text
cd extension && scons -Q test; cd ..
```

`645/645` native doctest cases and `9,120,150/9,120,150` assertions passed.

```text
./gdunit_tests.sh -a res://tests/test_connectivity.gd
```

`reports/report_51/results.xml` recorded 33/33 cases, 0 errors, 0 failures, and 0 flaky. This
includes both the consolidation-only no-stale case and the consolidated-away append regression.

### Refreshed exit scans

Run after the source fix:

```text
$ rg 'WorldStore::append_edit\(|append_edit_locked|struct EditSink|struct ConsolidationSink|on_edit_appended|LodSystem::note_edit' extension/src
# empty; exit 1

$ rg -il 'lock order' extension/src
extension/src/core/edit_pipeline.h
# exit 0

$ rg 'max_override_bricks' tests/test_connectivity.gd
# empty; exit 1

$ rg 'collect_ops_for_aabb|edit_log\(\)->ops\(' extension/src/physics
# empty; exit 1

$ git diff --check
# exit 0
```

The first, third, and fourth scans are empty as shown; the lock-order scan has exactly the single
intended owner. The scan evidence was refreshed after the prior header comment was reintroduced
by the previous review fix.

### Results/report metadata

`docs/superpowers/plans/2026-09-17-edit-pipeline-results.md` now records source fix commit
`ca30d9f`, the final focused report `reports/report_51`, and the same refreshed scan outputs.
The existing full-run evidence remains `reports/report_46`: it had the documented ambient
Godot/Metal error and one unresolved sun-cascade failure. The parent control and current rerun
failed different sun cases, so GPU attribution remains cautious; this final wave does not claim
that failure is unrelated to the branch.
