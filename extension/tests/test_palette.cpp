#include <doctest/doctest.h>
#include "world/palette.h"

TEST_CASE("palette inserts up to 4 materials and reuses slots") {
	uint16_t pal[4] = {0, 0, 0, 0};
	bool ovf = false;
	CHECK(ve::palette_slot(pal, 10, &ovf) == 0);
	CHECK(ve::palette_slot(pal, 20, &ovf) == 1);
	CHECK(ve::palette_slot(pal, 30, &ovf) == 2);
	CHECK(ve::palette_slot(pal, 40, &ovf) == 3);
	CHECK(ve::palette_slot(pal, 20, &ovf) == 1); // existing
	CHECK_FALSE(ovf);
}

TEST_CASE("palette overflow picks nearest existing id") {
	uint16_t pal[4] = {0, 0, 0, 0};
	bool ovf = false;
	ve::palette_slot(pal, 10, &ovf);
	ve::palette_slot(pal, 20, &ovf);
	ve::palette_slot(pal, 30, &ovf);
	ve::palette_slot(pal, 40, &ovf);
	CHECK(ve::palette_slot(pal, 38, &ovf) == 3); // nearest to 40
	CHECK(ovf);
	ovf = false;
	CHECK(ve::palette_slot(pal, 12, &ovf) == 0); // nearest to 10
	CHECK(ovf);
}
