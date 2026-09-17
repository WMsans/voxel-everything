# World field query — final review fix wave

Base: `df7a21d`

## Fixes

- Added `LodTree::Node::refused`; over-cap refusals suppress requests until `mark_dirty()` touches the node, while ordinary state transitions clear the suppression bit.
- Strengthened `test_lod_tree.cpp` to verify a fresh refused target is absent from the next walk and present after dirtying its chunk.
- Tightened `test_an_over_cap_lod_chunk_is_refused_not_truncated` to require `op_overflow == true`.

## Verification

- Red regression before the production fix: `623 passed | 1 failed`; the refused target was re-requested.
- `cd extension && scons -Q test`: `625 passed | 0 failed | 0 skipped`.
- `./build.sh -j$(sysctl -n hw.ncpu 2>/dev/null || nproc)`: build OK.
- S3/LoD gdUnit suites (`report_51`): `25 test cases | 0 errors | 0 failures` across 8 suites.

No results docs, goldens, or consumer pins were changed.
