# Task 33: Generated blocks for frame post passes, beauty camera, and sun light

Status: PASS for the Task 33 gates

## Changes

Migrated the specified frame post-pass, beauty-camera, and sun-light shader/C++ blocks to `shaders/generated/blocks.glslh` and the Task 32 generated C++ structs/macros:

- SSAO
- Contact shadow
- Outline
- SSR
- SSGI
- Hi-Z
- Deferred push constants and sun cascade UBO
- Beauty camera UBO
- Sun light UBO

Preserved bindings, layouts, values, dispatch ordering, barriers, and locks. Replaced the targeted hand-packed byte buffers and tautological size assertions with generated structs and `gpu::push_bytes`. The Task 33 hand-offset/static-assert scan is clean.

## Commits

- `58ad510` `refactor(ssao): push block from the generated layout`
- `a90948f` `refactor(contact_shadow): push block from the generated layout`
- `cdc23f7` `refactor(outline): push block from the generated layout`
- `d76fe91` `refactor(ssr): push block from the generated layout`
- `5dd1edc` `refactor(ssgi): push block from the generated layout`
- `1b6ef00` `refactor(hiz): push block from the generated layout`
- `8584b04` `refactor(deferred): push block from the generated layout`
- `c889687` `refactor(beauty_camera): block from the generated layout`
- `9735e4d` `refactor(sun_ubo): block from the generated layout`

## Tests

- Generation: `(cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests)` — PASS; 569/569 cases, 9,117,280/9,117,280 assertions.
- Native: `(cd extension && scons -Q test)` — PASS; 569/569 cases, 9,117,276/9,117,276 assertions.
- Sun-light native shader pin: 1/1 case, 5/5 assertions; 568 cases skipped by the filter.
- Build: `./build.sh -j8` — PASS; universal macOS extension linked. Only existing C++20-extension warnings appeared.
- Combined GPU gate: all 14 required suites — PASS; 99/99 cases, 0 errors, 0 failures.
- `git diff --check f2518ad..HEAD` — PASS.
- Required generated include/macro and no-hand-offset scans — PASS.

The separate umbrella command `./gdunit_tests.sh` was allowed 900 seconds but timed out before its overall summary. It produced no reported test failure before timeout; its incomplete log is retained at `.superpowers/sdd/2026-09-15-pass-anatomy-generated-layouts/task-33-full-gdunit.log`.

## Evidence

Per-pass and final gate logs are under `.superpowers/sdd/2026-09-15-pass-anatomy-generated-layouts/`, including `task-33-final-gate.log`, `task-33-generation.log`, `task-33-native-final.log`, and `task-33-build-final.log`.
