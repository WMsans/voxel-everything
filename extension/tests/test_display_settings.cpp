#include <doctest/doctest.h>
#include "settings/display_settings.h"
#include "settings_row_checks.h"
#include <cstdio>
#include <cstring>
#include <iterator>

TEST_CASE("the display rows satisfy the table invariants") {
	ve::DisplaySettings shipped; // project.godot: scaling_3d/scale=0.65 at 2560x1440
	shipped.render_scale = 0.65f;
	shipped.resolution = 3;
	check_rows(ve::display_rows(), {ve::DisplaySettings{}, shipped});
}

// Dropdown labels are what the player picks from; a label that disagreed with the size it applies
// would be silently wrong in the only place it is visible.
TEST_CASE("the resolution table is ascending and its labels name the sizes they apply") {
	const ve::SettingRow<ve::DisplaySettings> *row = ve::find_row(ve::display_rows(), "resolution");
	REQUIRE(row != nullptr);
	REQUIRE(row->options.size() == std::size(ve::kResolutions));
	CHECK(row->min == -1.0f);
	for (size_t i = 0; i < std::size(ve::kResolutions); i++) {
		char want[32];
		std::snprintf(want, sizeof(want), "%d x %d", ve::kResolutions[i].w, ve::kResolutions[i].h);
		CHECK(std::strcmp(row->options[i], want) == 0);
		if (i > 0) CHECK(ve::kResolutions[i].w > ve::kResolutions[i - 1].w);
	}
}

TEST_CASE("a window size maps to its preset index, or -1 when it is not a preset") {
	CHECK(ve::resolution_index_of(2560, 1440) == 3); // project.godot's shipped size
	CHECK(ve::resolution_index_of(1920, 1080) == 2);
	CHECK(ve::resolution_index_of(1337, 999) == -1);
}

TEST_CASE("the upscaler options match the mode table VoxelSettings maps them to") {
	const ve::SettingRow<ve::DisplaySettings> *row = ve::find_row(ve::display_rows(), "upscaler");
	REQUIRE(row != nullptr);
	CHECK(row->options.size() == static_cast<size_t>(ve::kUpscalerOptionCount));
}
