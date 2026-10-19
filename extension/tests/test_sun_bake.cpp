#include <doctest/doctest.h>
#include "render/sun_bake.h"

// The sun cache is baked per brick, so it is valid only for the sun it was baked under. A
// direction change is the only thing that invalidates every entry at once; the shipped demo
// never moves its sun, which is why nothing else exercises this.
TEST_CASE("sun_dir_changed: first frame, still, and a real turn") {
	const float zero[3] = {0, 0, 0}, a[3] = {0.5746958f, 0.7662610f, 0.2873479f};
	const float a_jitter[3] = {0.5746960f, 0.7662608f, 0.2873480f};
	const float b[3] = {0.0f, 1.0f, 0.0f};
	CHECK(ve::sun_dir_changed(zero, a));
	CHECK_FALSE(ve::sun_dir_changed(a, a_jitter));
	CHECK(ve::sun_dir_changed(a, b));
}
