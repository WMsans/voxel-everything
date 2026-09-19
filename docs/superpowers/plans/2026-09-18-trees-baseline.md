# Trees — baseline

Commit: `040e209` on branch `feat/trees` (worktree `.worktrees/trees`), tree clean.
Recorded 2026-09-18 on Darwin Jeremys-Mac-mini.attlocal.net / Apple M1 / Godot 4.7.2.stable.official.ed1daf0bf.
gdUnit report: `reports/report_1` (git-ignored via `.gitignore` `reports/`; not committed).

Full captured output of the gdUnit run this doc is derived from: `/tmp/gdunit-baseline.txt`.

## Native (doctest, `./build.sh --test`)

All pass. No native failures to baseline.

```
[doctest] test cases:     683 |     683 passed | 0 failed | 0 skipped
[doctest] assertions: 9127512 | 9127512 passed | 0 failed |
[doctest] Status: SUCCESS!
```

## gdUnit (`./gdunit_tests.sh`, exit code 100)

There ARE pre-existing failures; the two cases below are the entire failure set.

```
Overall Summary: 509 test cases | 1 errors | 1 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (96/96)
Executed test cases : (509/509)
Total execution time: 18min 21s 399ms
Exit code: 100
```

Suite-level stats of the two suites that contributed failures (all other 94 suites: 0 errors, 0 failures):

- `res://tests/test_voxel_settings.gd` — `Statistics: 17 test cases | 1 errors | 0 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 124ms`
- `res://tests/test_sun_cascades_gpu.gd` — `Statistics: 3 test cases | 0 errors | 1 failures | 0 flaky | 0 skipped | 0 orphans | PASSED 36s 317ms`

## gdUnit failing cases (verbatim)

Failure messages below are verbatim after stripping ANSI colour codes (`perl -pe 's/\e\[[0-9;]*m//g'`).
They contain literal TAB characters (shown as the `\t` gap before `at`); do not normalize them away.

### 1. `res://tests/test_voxel_settings.gd > test_an_ambient_change_reaches_the_object_global`

Counted by gdUnit as the run's 1 **error**. Result line:

```
  res://tests/test_voxel_settings.gd > test_an_ambient_change_reaches_the_object_global FAILED 15ms
```

gdUnit failure message (the `Report:` block):

```
  Report:
  Godot Runtime Error !
  'Trying to assign value of type 'Nil' to a variable of type 'Vector3'.'	at 'test_an_ambient_change_reaches_the_object_global' in res://tests/test_voxel_settings.gd:213
```

Accompanying engine output immediately before the FAILED line (verbatim):

```
ERROR: This function should never be used outside the editor, it can severely damage performance.
   at: global_shader_parameter_get (servers/rendering/renderer_rd/storage_rd/material_storage.cpp:1913)
   GDScript backtrace (most recent call first):
       [0] test_an_ambient_change_reaches_the_object_global (res://tests/test_voxel_settings.gd:213)
SCRIPT ERROR: Trying to assign value of type 'Nil' to a variable of type 'Vector3'.
          at: test_an_ambient_change_reaches_the_object_global (res://tests/test_voxel_settings.gd:213)
          GDScript backtrace (most recent call first):
              [0] test_an_ambient_change_reaches_the_object_global (res://tests/test_voxel_settings.gd:213)
```

Known environment error, already flagged in `2026-09-17-edit-pipeline-baseline.md`: Godot/Metal
refuses `global_shader_parameter_get` outside the editor and returns Nil, so the assignment at
`test_voxel_settings.gd:213` is a runtime error, not an assertion failure.

### 2. `res://tests/test_sun_cascades_gpu.gd > test_sub_texel_motion_rebuilds_no_cascade`

Counted by gdUnit as the run's 1 **failure**. Result line:

```
  res://tests/test_sun_cascades_gpu.gd > test_sub_texel_motion_rebuilds_no_cascade FAILED 35s 223ms
```

gdUnit failure message (the `Report:` block):

```
  Report:
  Expecting: 'true' but is 'false'	at 'test_sub_texel_motion_rebuilds_no_cascade' in res://tests/test_sun_cascades_gpu.gd:73
	at 'settle' in res://tests/test_sun_cascades_gpu.gd:41
```

Accompanying engine output immediately before the FAILED line. The trigger is a `push_error` of a
single 217,545-character line: the world-diagnostics dump embedded in it (`draw_page_ids`, a
13,269-entry array, and a pending-ops string array) is run-variable, so the exact byte-for-byte
line is not reproducible and is not baselined. Its verbatim start (through `"draw_page_ids": [`)
and verbatim end are recorded below; the numeric field values shown are from this baseline run
and a matching later run is not required to reproduce them digit-for-digit:

```
ERROR: strict settle timeout: { "pages_total": 32768, "pages_free": 12459, "pages_used": 20309, "chunks_resident": 6107, "chunk_records": 8192, "chunk_records_used": 6107, "chunk_records_high_water": 6148, "pages_high_water": 20348, "budget_bound": "none", "dirty_chunks": 0, "dirty_levels": 0, "draw_pages": 13269, "draw_page_ids": [
```

```
…<run-variable page-id and pending-op arrays>…], "op_overflow": 0, "partial_allocations": 0, "builds_in_flight": 1, "lod_pending": 32, "culled_ratio": 0.0 }
   at: push_error (core/variant/variant_utility.cpp:1023)
   GDScript backtrace (most recent call first):
       [0] settle (res://tests/test_sun_cascades_gpu.gd:40)
```

## How to answer "did I break this?" after a later run

1. Re-run: `./build.sh --test 2>&1 | tail -40` (must stay 683/683) and
   `./gdunit_tests.sh 2>&1 | tee /tmp/gdunit-run.txt | tail -60`.
2. Normalize: `perl -pe 's/\e\[[0-9;]*m//g' /tmp/gdunit-run.txt > /tmp/gdunit-run.clean.txt`,
   then `grep -n "FAILED" /tmp/gdunit-run.clean.txt` and read each case's `Report:` block.
3. A later FAILED case is PRE-EXISTING only if BOTH its case name AND its verbatim `Report:`
   failure message match one of the two cases above. Same case name with a different message is a
   NEW failure wearing an old name. Any FAILED case not named above is new. Expected steady-state
   totals: `509 test cases | 1 errors | 1 failures` (case counts may legitimately grow as tests
   are added; the failure set must not).
