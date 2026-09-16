#include <doctest/doctest.h>
#include "settings/render_settings.h"
#include "settings_row_checks.h"
#include "shade/beauty_settings_store.h"

TEST_CASE("the render rows satisfy the table invariants") {
	check_rows(ve::render_rows(), {ve::RenderSettings{}});
}

// RenderOrchestrator's atomics started at these values; the store's defaults must match them.
TEST_CASE("render defaults are the dials the orchestrator shipped with") {
	const ve::RenderSettings s;
	CHECK(s.quality_tier == static_cast<int>(ve::QualityTier::kHigh));
	CHECK(s.near_field_scale == doctest::Approx(0.66f));
	CHECK(s.near_field);
	CHECK(s.islands);
}

TEST_CASE("the render dials clamp as the orchestrator's setters did") {
	ve::RenderSettingsStore store;
	CHECK(store.set_value("quality_tier", 9.0f));
	CHECK(store.get().quality_tier == 3);
	CHECK(store.set_value("quality_tier", -2.0f));
	CHECK(store.get().quality_tier == 0);
	CHECK(store.set_value("near_field_scale", 5.0f));
	CHECK(store.get().near_field_scale == doctest::Approx(1.0f));
	CHECK(store.set_value("near_field_scale", 0.0f));
	CHECK(store.get().near_field_scale == doctest::Approx(0.1f));
}
