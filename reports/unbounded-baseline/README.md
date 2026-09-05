# Pre-unbounded-world baseline

Recorded before any behaviour change (Task 1). A later task that sees a
failure not on this list has caused it.

- git SHA: `55113b56e4661030fe95b7087827682f9ccbf45a`
  (`55113b5 docs: unbounded world implementation plan`)
- Native suite command: `cd extension && scons test -j8`
  (full log: `native.txt`)
- gdUnit suite command: `./gdunit_tests.sh`
  (full log: `gdunit.txt`; exit code 100, `|| true` per brief)

## Native suite (doctest) — verbatim

```
[doctest] test cases:     426 |     426 passed | 0 failed | 0 skipped
[doctest] assertions: 9812754 | 9812754 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Zero native failures.

## gdUnit suite — verbatim summary

```
Overall Summary: 378 test cases | 0 errors | 5 failures | 0 flaky | 0 skipped | 0 orphans |
Executed test suites: (74/74)
Executed test cases : (378/378)
Exit code: 100
```

## Every failing gdUnit assertion (verbatim, with suite name)

### 1. res://tests/test_collider_octants.gd — test_split_colliders_still_match_the_field

```
res://tests/test_collider_octants.gd > test_split_colliders_still_match_the_field FAILED 613ms
Report:
Expecting: 'true' but is 'false'	at 'test_split_colliders_still_match_the_field' in res://tests/test_collider_octants.gd:41
```

### 2. res://tests/test_contact_shadow.gd — test_a_crater_darkens_its_own_floor

```
res://tests/test_contact_shadow.gd > test_a_crater_darkens_its_own_floor FAILED 601ms
Report:
no pixel was occluded at all: the march never hit anything	at 'test_a_crater_darkens_its_own_floor' in res://tests/test_contact_shadow.gd:41
```

### 3. res://tests/test_island_body.gd — test_a_body_lands_on_the_streamed_collider_and_sleeps (2 failed assertions)

```
res://tests/test_island_body.gd > test_a_body_lands_on_the_streamed_collider_and_sleeps FAILED 7s 167ms
Report:
the body never came to rest on the terrain	at 'test_a_body_lands_on_the_streamed_collider_and_sleeps' in res://tests/test_island_body.gd:69
Expecting to be greater than:
47.552086 but was -118.597923	at 'test_a_body_lands_on_the_streamed_collider_and_sleeps' in res://tests/test_island_body.gd:73
```

### 4. res://tests/test_collider_stream.gd — test_colliders_appear_around_the_player

```
res://tests/test_collider_stream.gd > test_colliders_appear_around_the_player FAILED 610ms
Report:
Expecting:
0
but was
63	at 'test_colliders_appear_around_the_player' in res://tests/test_collider_stream.gd:89
```

Total: 4 failing test cases, 5 failing assertions. Everything else (374/378 cases) passed.

## Environment notes (setup only, no behaviour change)

Recording the baseline in this worktree required environment setup that is
NOT part of the baseline diff (all outputs are git-ignored build artefacts):

- `git submodule update --init extension/godot-cpp` (submodule was uninitialised;
  top-level `SConstruct` cannot even parse without it, blocking `scons test`).
- `godot --path . --import` (generates git-ignored `.godot/`, incl. the global
  script class cache the gdUnit runner needs to resolve `GdUnitTestCIRunner`).
- `cd extension && scons -j8` (builds git-ignored `extension/bin/` native
  library; without it every `VoxelWorld`-referencing suite fails to parse and
  the run exits 105 with zero tests executed).
