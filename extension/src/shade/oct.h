#pragma once

namespace ve {

// Octahedral normal encoding. The G-buffer's surface target stores the two components in
// fp16, which is why the range is [-1, 1] rather than [0, 1]: the sign bit is free there and
// halving the range would throw away a bit of precision for nothing.
//
// shaders/shade.glslh mirrors both functions. Task 6's differential test diffs a GPU
// round trip against this one and fails when they drift.
void oct_encode(const float n[3], float out[2]);
void oct_decode(const float e[2], float out[3]);

} // namespace ve
