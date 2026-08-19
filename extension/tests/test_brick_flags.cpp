#include <doctest/doctest.h>
#include "world/brick_flags.h"
#include "world/brick_mip.h"

namespace {

// A mip chain whose whole 2^3 level sits on one side of zero: no surface anywhere.
ve::BrickMips uniform_mips(uint8_t value) {
	ve::BrickMips m;
	for (int i = 0; i < 8; i++) { m.mn2[i] = value; m.mx2[i] = value; }
	for (int i = 0; i < 64; i++) { m.mn4[i] = value; m.mx4[i] = value; }
	for (int i = 0; i < 512; i++) { m.mn8[i] = value; m.mx8[i] = value; }
	return m;
}

} // namespace

TEST_CASE("a brick straddling zero anywhere reports a surface") {
	ve::BrickMips m = uniform_mips(200); // all air
	CHECK(ve::brick_flags_from_mips(m, 1) == 0u + ve::kBrickFlagHasMaterial);
	// One cell of the 2^3 level crossing zero is enough: the marcher must enter the brick.
	m.mn2[5] = 10;
	m.mx2[5] = 250;
	CHECK(ve::brick_flag_has_surface(ve::brick_flags_from_mips(m, 1)));
}

TEST_CASE("an empty palette means no material even when the field crosses zero") {
	// Palette slot 0 holding id 0 is how a brick says "nothing here has a material"; the
	// marcher's hit test refuses such a brick, and the flag has to agree or the DDA would
	// enter it, sphere-trace it, and reject the hit after paying for all of it.
	ve::BrickMips m = uniform_mips(128);
	const uint32_t flags = ve::brick_flags_from_mips(m, 0);
	CHECK(ve::brick_flag_has_surface(flags));
	CHECK_FALSE(ve::brick_flag_has_material(flags));
}

TEST_CASE("the conservative value marches everything") {
	CHECK(ve::brick_flag_has_surface(ve::kBrickFlagConservative));
	CHECK(ve::brick_flag_has_material(ve::kBrickFlagConservative));
}

TEST_CASE("the flag agrees with the eight-cell reduction it replaces") {
	// The property that makes the swap safe: for every mip chain, the flag is exactly what
	// the marcher's old reduce-over-eight-cells test computed.
	ve::BrickMips m = uniform_mips(128);
	for (int trial = 0; trial < 8; trial++) {
		m.mn2[trial] = static_cast<uint8_t>(trial * 30);
		m.mx2[trial] = static_cast<uint8_t>(255 - trial * 20);
		uint8_t mn = 255, mx = 0;
		for (int i = 0; i < 8; i++) {
			mn = mn < m.mn2[i] ? mn : m.mn2[i];
			mx = mx > m.mx2[i] ? mx : m.mx2[i];
		}
		const bool old_test = ve::mip_cell_has_surface(mn, mx);
		CHECK(ve::brick_flag_has_surface(ve::brick_flags_from_mips(m, 1)) == old_test);
	}
}
