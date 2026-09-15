# Task 14 final review fix report

Status: `DONE_WITH_CONCERNS` (documentation-only review fix wave)

Parent revision: `a153859b45e4d4d5d999c41eafaf43e2a8418238` (`docs: render lifetime owner results and roadmap amendments`)
Implementation revision documented by the results report: `48eb783e100ef649fb23315e044c51945bb99f6b`.
The single fix commit contains this report and the two amended documentation files.

## Findings fixed

1. `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md` now distinguishes the implementation revision `48eb783`, the results/spec amendment `a153859`, the source baseline `ca9b87b`, and its evidence-log child `5f2d897`, which is the revision selected by the line-count command.
2. The SP1 roadmap acceptance row and the Task 11 historical wording in `docs/superpowers/plans/2026-09-13-frame-module.md` now state that `FrameHost` was deleted by SP2 in `6b595c1`. The Task 11 planning-stage wording records that it was temporary debt at that earlier stage without leaving the final status contradictory.
3. The results report records native exit status as unavailable. Existing evidence contains the doctest success output but did not capture a shell exit status, so no exit code is inferred.
4. This file is the required full Task 14 report.

## Changed files

- `docs/superpowers/plans/2026-09-13-frame-module.md`
- `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md`
- `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-14-report.md`

No production or addon files changed.

## Focused verification

No build, native suite, gdUnit suite, or broad test suite was rerun, per the review-fix instruction.

Command:

```bash
git diff --check && echo 'git diff --check: 0'
```

Output:

```text
git diff --check: 0
```

Command:

```bash
rg -n '48eb783|a153859|ca9b87b|5f2d897|Native exit status|FrameHost.*deleted by SP2|FrameHost.*deleted by sub-project 2|results report agree' docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md docs/superpowers/plans/2026-09-13-frame-module.md
```

Output:

```text
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:4:implementation revision `48eb783` (`refactor: split debug hooks by module (pure move)`).
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:5:The results/spec amendment was committed separately as `a153859` (`docs: render lifetime
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:12:| SP2 source baseline | `ca9b87b` (`refactor: consolidate pass-probe inputs and remove orphaned frame plumbing`) |
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:13:| Baseline evidence-log child | `5f2d897` (`docs: render lifetime owner baseline failure set`); the line-count command's `git log --grep` resolves to this child of `ca9b87b` |
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:14:| Final implementation revision | `48eb783e100ef649fb23315e044c51945bb99f6b` |
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:15:| Results/spec amendment revision | `a153859b45e4d4d5d999c41eafaf43e2a8418238` |
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:50:Native exit status: unavailable in the existing evidence. The recorded command did not append
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:78:Evidence sources: Task 1 commit `78d7cac`, Task 2 final commit `ca9b87b`, Task 10
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:93:| Spec, implementation status and results agree; `FrameHost` was deleted by SP2 | PASS (docs/static) | SP2 deleted `FrameHost` in commit `6b595c1`; the roadmap wording and status amendments record that deletion. Runtime gates remain open. |
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:167:The required command used baseline `5f2d897` and printed:
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:281:- Native exit status is marked unavailable because existing evidence did not capture it.
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md:282:- `FrameHost` is described consistently as deleted by SP2 (`6b595c1`), not as remaining debt.
docs/superpowers/plans/2026-09-13-frame-module.md:3101:- [x] Spec, implementation status and results report agree; `FrameHost` was deleted by sub-project 2 in commit `6b595c1`.
```

Command:

```bash
git diff --name-only && printf 'forbidden-production-files=' && git diff --name-only -- extension addons tests | wc -l
```

Output before adding this report:

```text
docs/superpowers/plans/2026-09-13-frame-module.md
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md
forbidden-production-files=       0
```

The final staged check was run after adding this report:

```bash
git diff --cached --check && echo 'git diff --cached --check: 0'
```

Output:

```text
git diff --cached --check: 0
```

The same staged command also reported these staged paths and no forbidden production paths:

```text
.superpowers/sdd/2026-09-14-render-lifetime-owner/task-14-report.md
docs/superpowers/plans/2026-09-13-frame-module.md
docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md
staged-forbidden-production-files=       0
```

Command:

```bash
if rg -n 'FrameHost remains named temporary debt|its deletion remains sub-project 2 debt|FrameHost removal remains explicitly assigned' docs/superpowers/plans/2026-09-13-frame-module.md; then exit 1; else echo 'stale FrameHost debt wording: none'; fi
```

Output:

```text
stale FrameHost debt wording: none
```

## Self-review and concerns

- Scope is limited to the two requested plan/report amendments plus this required report.
- No production, addon, shader, test, launcher, or generated engine file was changed.
- The native suite was not rerun; its exact exit status remains unavailable in the pre-existing evidence, while the doctest output recorded `551/551` passing cases and `9,117,142/9,117,142` passing assertions.
- gdUnit remains blocked before discovery by the pre-existing `GdUnitTestCIRunner` parse error; no runtime pass is claimed.
- The results report continues to list open runtime gates and the literal historical orphan-comment grep finding rather than converting static documentation fixes into runtime acceptance.

## Final fix wave — review findings 1–6

Status: `DONE_WITH_CONCERNS`.

Review base: final docs head `97b1c87` (`docs: apply Task 14 final review fixes`). This
follow-up keeps the wave minimal and does not modify addons, launcher infrastructure, pass
internals, shaders, APIs, locks, or frame stage order.

### Findings resolved

1. The results report now explicitly labels gdUnit discovery, lifetime, shipped-golden,
   frame-contract, S8, and lifetime-bite verification as **OPEN — external verification**.
   The pre-existing `GdUnitTestCIRunner` parse error remains an external blocker; no runtime
   pass is claimed and no addon/infrastructure file was changed.
2. The Fog retrace remains truthfully **OPEN** at 9 timing-aware files, one over the ≤8 target,
   because `gpu_timings.cpp` registration is required. No workaround or sub-project 4
   architecture change was made.
3. The two DeferredPass teardown/initialize pairs in `hooks_render.cpp` are documented as
   pre-existing calls from `hooks.cpp`, moved verbatim by Task 13. The global constraint
   forbids new lifecycle calls; the pure-move evidence shows the sequence stayed 4 calls to 4,
   and no ContactShadowPass lifecycle call was added or rerouted.
4. `extension/src/debug/hooks_common.h` now includes `<cstring>` directly for `std::memcpy`.
5. The stale FrameHost wording in `docs/superpowers/specs/2026-09-13-frame-module-design.md`
   now records the resolved deletion in `6b595c1`. Former `VoxelWorld`/`region_window()` API
   comments were corrected without behavior changes.
6. The results report names final docs head `97b1c87` explicitly.

### Changed files

- `extension/src/debug/hooks_common.h`
- `extension/src/core/world_store.h` (comment only)
- `extension/src/render/grass_scatter_pass.cpp` (comment only)
- `extension/src/render/grass_scatter_pass.h` (comment only)
- `docs/superpowers/specs/2026-09-13-frame-module-design.md`
- `docs/superpowers/plans/2026-09-14-render-lifetime-owner-results.md`
- `.superpowers/sdd/2026-09-14-render-lifetime-owner/task-14-report.md`

### Focused verification

The requested broad suites were not rerun.

```text
$ python3 direct-include check
hooks_common direct <cstring>: PASS

$ DeferredPass pure-move/lifecycle check
DeferredPass lifecycle calls: pre-existing sequence preserved (4 -> 4)
new ContactShadowPass lifecycle calls: 0

$ stale wording check
stale wording check: PASS

$ git diff --check
 diff-check=0

$ ./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc); echo build=$?
==> Build OK: 3.2M libvoxel_everything.macos.template_debug.universal.dylib
build=0
```

Concern: gdUnit runtime verification remains open until the external
`GdUnitTestCIRunner` launcher parse error is repaired outside this task.
