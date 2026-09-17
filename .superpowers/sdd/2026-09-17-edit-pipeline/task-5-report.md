# Task 5 Report: EditPipeline and merge_or_cap

## Status

Complete. The implementation follows the brief's public interfaces and behavior.

## RED evidence

Added `extension/tests/test_edit_pipeline.cpp` before production code, then ran:

```text
cd extension && scons -Q test
```

The test compilation failed as expected because the module header did not yet exist:

```text
tests/test_edit_pipeline.cpp:2:10: fatal error: 'core/edit_pipeline.h' file not found
scons: *** [tests/test_edit_pipeline.o] Error 1
Command exited with code 2
```

## GREEN evidence

Implemented the smallest module using existing `EditLog`, edit-op, region, and AABB helpers.

Verification after the Round 1 fixes is recorded below.

## Files

- `extension/src/core/edit_pipeline.h` — public pipeline/invalidation API, lock-order statement, `Box3`, and `merge_or_cap`.
- `extension/src/core/edit_pipeline.cpp` — consolidation invalidation, preflight, apply, sink management, and recording.
- `extension/tests/test_edit_pipeline.cpp` — the complete native contract test.

## Self-review

- Atomic preflight accounts for additions from all ops before append and returns full regions without mutation.
- Non-atomic application preserves partial acceptance, reports rejection invalidations, and increments the sequence once per changed op.
- Malformed, oversized, fully rejected, and missing-log cases are fail-soft as specified.
- Sink notifications carry sequence, AABB, append result, and `notify_islands`; sink registration is null-safe and duplicate-safe.
- Consolidation invalidations use region-derived world bounds and do not bump the edit sequence.
- `merge_or_cap` absorbs transitive overlaps and folds the queue into one bounding box after the cap.
- The lock-order statement appears only in the new header.

## Concerns

None within Task 5 scope. The pipeline has no callers yet, as stated in the brief; callers must honor the documented edit-lock contract.

## Commit

`f07e936 feat: one edit pipeline with atomic batches and invalidation sinks`

## Round 1 Fix Report

### Reviewer disposition

- Repository-wide lock-order restatements were intentionally left unchanged; Task 13 owns that cleanup.
- `Invalidation::op` and `Invalidation::append` are now documented as borrowed only for the synchronous `record()` call. A deferred sink must copy the pointed-to `EditOp` and `AppendResult`, including vectors, before returning and must not retain either pointer.
- Native coverage now proves one accepted invalidation reaches two sinks and one fully accepted cross-region op increments the sequence exactly once.

### Overflow fix TDD

The root cause was signed `int` subtraction in `op_region_span()`. A finite sphere with radius `5.48e10f` produces valid region endpoints near opposite `int` limits; subtracting them overflowed, returned `-13717438`, and made `op_region_span_ok()` incorrectly accept the op. `EditLog::append()` would then enter its region loops.

RED: added `an extreme finite sphere span is rejected without signed overflow` before changing production code, then ran:

```text
cd extension && scons -Q test
```

```text
[doctest] test cases:     642 |     641 passed | 1 failed | 0 skipped
[doctest] assertions: 9120135 | 9120133 passed | 2 failed |
[doctest] Status: FAILURE!
```

The failure was the expected regression failure:

```text
CHECK( -13717438 >  64 ) is NOT correct!
CHECK_FALSE( true ) is NOT correct!
```

GREEN: `op_region_span()` now computes in `uint64_t` and clamps only at the public `int` boundary. This preserves normal spans and makes the existing `EditLog::append()` oversized guard fail closed.

### Corrected verification

The prior `--test-case='*pipeline*'` report did not match all Task 5 test names. The reproducible focused command is source-based:

```text
./extension/build/tests/ve_tests --source-file='*test_edit_pipeline.cpp'
```

```text
[doctest] test cases: 18 | 18 passed | 0 failed | 626 skipped
[doctest] assertions: 90 | 90 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Overflow regression:

```text
./extension/build/tests/ve_tests --test-case='*extreme finite sphere*'
```

```text
[doctest] test cases: 1 | 1 passed | 0 failed | 643 skipped
[doctest] assertions: 2 | 2 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Full native suite:

```text
cd extension && scons -Q test
```

```text
[doctest] test cases:     644 |     644 passed | 0 failed | 0 skipped
[doctest] assertions: 9120146 | 9120146 passed | 0 failed |
[doctest] Status: SUCCESS!
```

Extension build:

```text
./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)
```

```text
==> Build OK: 3.2M libvoxel_everything.macos.template_debug.universal.dylib
==> Done.
```

### Commits

- `66d687f fix: guard region span arithmetic against overflow`
- `8e2b865 fix: clarify edit pipeline invalidation contract`

## Round 2 Fix Note

Removed the superseded `--test-case='*pipeline*'` command/output from the initial GREEN evidence. The source-file command below is the single reproducible focused-suite evidence; the final full native suite remains 644/644.
