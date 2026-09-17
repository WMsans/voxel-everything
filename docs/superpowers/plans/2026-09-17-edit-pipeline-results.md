# Edit pipeline (sub-project 5b) — results

Implementation baseline tested through: `30d2838`. Documentation commit: this commit.
Recorded 2026-09-17 on Darwin Jeremys-Mac-mini.attlocal.net (Apple M1 / Metal 4, Godot 4.7.2.stable.official.ed1daf0bf).
Baseline: `docs/superpowers/plans/2026-09-17-edit-pipeline-baseline.md`. Report: `reports/report_46`.

## Regression
Build: `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)` — Build OK.

Native: `[doctest] test cases:     645 |     645 passed | 0 failed | 0 skipped`

gdUnit: 509 executed cases / 1 error / 1 failure; 96/96 suites and 509/509 cases executed in `reports/report_46/results.xml`; exit code 100. Differences from baseline:
- `test_edit_fanout`: new suite, 4 cases, all passed.
- `test_connectivity`: current declared count 33, same as baseline. It has two Task 10 staleness cases (the Task 3 pin renamed by Task 10 and the consolidated-away-append regression) and loses the two Task 9 cases: `test_post_spawn_carve_rejection_keeps_body_in_hole` and `test_near_cap_carve_is_refused_before_any_carve`. The hold seam is a fixture change, not a new case.
- `test_consolidation`: +2 cases, `test_a_full_op_list_does_not_stay_full_while_consolidation_runs` and `test_a_held_consolidation_keeps_a_full_op_list_full`.
- `test_repro_pillar_debris`: same count, its landing dump loses `land_carve_nothing` and `land_carve_restored`.
- `test_sun_cascades_gpu`: same declared count (7), baseline had 2 failures and the full run had 1. The current failure was `test_sub_texel_motion_rebuilds_no_cascade — FAILED: res://tests/test_sun_cascades_gpu.gd:73`; the parent control retained one failure in `test_the_min_level_clamp_does_not_peter_pan` (see Concerns), so this is GPU/environment drift, not an edit-pipeline failure.
- `test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213` is unchanged and remains the documented Godot/Metal `Nil` environment error.
- All other suites kept their declared counts and failure/error sets.

The full Task 1 extraction script was run against `reports/report_46/results.xml`. Its failure/error output was:

```text
test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
test_sun_cascades_gpu::test_sub_texel_motion_rebuilds_no_cascade — FAILED: res://tests/test_sun_cascades_gpu.gd:73
```

The baseline markdown extraction contains:

```text
test_voxel_settings::test_an_ambient_change_reaches_the_object_global — ERROR: res://tests/test_voxel_settings.gd:213
test_sun_cascades_gpu::test_the_min_level_clamp_does_not_peter_pan — FAILED: res://tests/test_sun_cascades_gpu.gd:101
```

The exact declared-count comparison was: baseline 95 suites / 507 declared tests; current 96 suites / 513 declared tests. The planned count additions are the 4-case fan-out suite and 2 consolidation cases; connectivity is net zero because the two Task 10 staleness cases replace the two deleted Task 9 cases. The runner's executed summaries are 504 baseline cases and 509 current cases because failed-suite execution accounting differs from declared XML counts.

## Pins
| Pin | Result |
|---|---|
| `EDITS_GOLDEN` | unchanged |
| `FORCED_COMMIT_GOLDEN` | unchanged |
| `ASYNC_COMMIT_GOLDEN` | unchanged |
| `LOD_GOLDEN` | unchanged |
| staleness (`test_connectivity`) | moved in `f24472d` / extended by `a42b36e` — a bake appends nothing, so an in-flight extraction is no longer stale; the conservative append-sequence regression is also pinned |
| `test_frame_shipped_golden` | unchanged |

## Lock edges removed
| Edge | Removed in |
|---|---|
| edit → `LodSystem::mutex()` | `6506305` |
| edit → `IslandManager::windows_mutex_` (mutex deleted) | `a725648` |

## Sinks (the record() rule)
- `extension/src/voxel_world.cpp:849` — queues accepted edit/consolidation collider chunk ranges in `pending_dirty_`; for rejected edits increments `edit_rejections` and logs the existing diagnostics; takes no mutex.
- `extension/src/physics/island_manager.cpp:154` — copies accepted island edits and sequence numbers into `inbox_`; takes no mutex.
- `extension/src/core/world_store.cpp:31` — copies accepted edits and append results into `pending_edits_`; takes no mutex.
- `extension/src/mesh/consolidation.cpp:20` — queues consolidated-region regeneration or accepted-edit consolidation regions; takes no mutex.
- `extension/src/lod/lod_system.cpp:368` — merges edit/consolidation boxes into `pending_marks_`; takes no mutex.

Each body was read statically. No `record()` body acquires a mutex; each only updates its own edit-lock-guarded queue, except the VoxelWorld stats half, which increments a counter and logs.

## Exit checks
Command 1:
```text
$ rg 'WorldStore::append_edit\(|append_edit_locked|struct EditSink|struct ConsolidationSink|on_edit_appended|LodSystem::note_edit' extension/src
```
Output: empty; exit 1.

Command 2:
```text
$ rg -il 'lock order' extension/src
extension/src/core/edit_pipeline.h
```
Exit 0.

Command 3:
```text
$ rg 'max_override_bricks' tests/test_connectivity.gd
```
Output: empty; exit 1.

Command 4:
```text
$ rg 'collect_ops_for_aabb|edit_log\(\)->ops\(' extension/src/physics
```
Output: empty; exit 1.

## Change cost (spec §8)
| Scenario | Before | After | Files after |
|---|---|---|---|
| A new consumer that must hear about edits | 5 | 2 | the consumer's `.h` and `.cpp` |

The after path is the consumer's `record()`/drain implementation plus one registration site; the registration site is not an additional consumer-owned file.

## Deletions
`WorldStore::append_edit{,_locked}`, `EditSink`, `ConsolidationSink`, `WorldStore::bump_edit_seq`,
`VoxelWorld::append_edit_locked`, `VoxelWorld::on_edit_appended`, `LodSystem::note_edit` (public),
`Collaborators::append_edit_locked`, `ConsolidationCoordinator::Collaborators::{lod_tree, lod_mutex,
pending_dirty}`, `LodSystem::{tree_slot, mutex_slot}`, `IslandManager::windows_mutex_`, the post-carve
restore branch with `debug_set_fail_next_carve` / `debug_set_fail_next_restore`, and
`test_connectivity.gd`'s `max_override_bricks = 1`.

## Open
`extension/src/debug/hooks_physics.cpp:942` (`debug_mesh_submit`), handed over by 5a, is a diagnostic mesh-job op copy like its 5a siblings, not part of the edit spine; it stays.

The final full run's non-clean status is limited to the unchanged ambient Metal error and GPU sun-cascade drift. The required parent control was rebuilt at `a725648` and reran with `./gdunit_tests.sh -a res://tests/test_sun_cascades_gpu.gd`: it passed the sub-texel case and failed one `test_the_min_level_clamp_does_not_peter_pan` assertion (`Expecting: 'true' but is 'false'` at line 108), confirming the suite-level GPU instability. A current-commit rerun (`reports/report_48`) failed earlier during shader compilation (`unexpected LEFT_OP`) with 1 executed case and 8 failures; no passing data is claimed from it.

Deferred ledger items were triaged as follows: the Task 1 baseline commit-label mismatch, Task 2 omitted empty-golden failure transcript, Task 5 report wording, and Task 12 structural-TDD label are historical documentation/evidence defects with no production impact; Task 6's registration/removal concurrency remains review-only rather than directly tested; Task 9's store-before-preflight comment mismatch remains a minor documentation concern. The Task 4 one-brick wording and Task 8 partial-carve comments were superseded by the later hold/atomic-carve changes. No additional edit-pipeline exit finding remains.
