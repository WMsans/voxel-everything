#pragma once

namespace ve {

// True when the sun has moved enough that every baked sun entry is stale: the angle between
// the two directions exceeds 0.1 degrees, or `prev` is all zeros (nothing has been baked yet,
// i.e. the first frame). Both directions are normalized.
//
// Lives apart from SunBakePass because the native (zero-godot-cpp) test suite compiles this
// header; the pass includes it and calls it from note_sun().
inline bool sun_dir_changed(const float prev[3], const float cur[3]) {
	if (prev[0] == 0.0f && prev[1] == 0.0f && prev[2] == 0.0f) return true;
	// cos(0.1 degrees)
	constexpr float kCosLimit = 0.9999984769132877f;
	const float dot = prev[0] * cur[0] + prev[1] * cur[1] + prev[2] * cur[2];
	return dot < kCosLimit;
}

} // namespace ve
