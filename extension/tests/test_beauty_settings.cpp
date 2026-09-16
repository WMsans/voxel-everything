#include <doctest/doctest.h>
#include <initializer_list>
#include "settings_row_checks.h"
#include "shade/beauty_settings.h"
#include "shade/beauty_settings_store.h"

TEST_CASE("the Off tier turns every effect off and leaves no work in any counter") {
	const ve::BeautySettings s = ve::settings_for_tier(ve::QualityTier::kOff);
	CHECK_FALSE(s.ssgi);
	CHECK_FALSE(s.ssr);
	CHECK_FALSE(s.contact_shadows);
	CHECK_FALSE(s.outlines);
	CHECK_FALSE(s.sun_shadow_map);
	CHECK_FALSE(s.glossy_sdf_rays);
	CHECK_FALSE(s.raymarched_sun_shadow);
	CHECK(s.ssgi_taps == 0);
	CHECK(s.ssr_steps == 0);
	CHECK(s.contact_steps == 0);
}

TEST_CASE("the tiers are ordered: nothing gets cheaper as quality rises") {
	const ve::BeautySettings lo = ve::settings_for_tier(ve::QualityTier::kLow);
	const ve::BeautySettings me = ve::settings_for_tier(ve::QualityTier::kMedium);
	const ve::BeautySettings hi = ve::settings_for_tier(ve::QualityTier::kHigh);
	CHECK(me.ssgi_taps >= lo.ssgi_taps);
	CHECK(hi.ssgi_taps >= me.ssgi_taps);
	CHECK(me.ssr_steps >= lo.ssr_steps);
	CHECK(hi.ssr_steps >= me.ssr_steps);
	CHECK(me.contact_steps >= lo.contact_steps);
	CHECK(hi.contact_steps >= me.contact_steps);
}

TEST_CASE("High is the demo default and matches the fixed numbers table") {
	const ve::BeautySettings hi = ve::settings_for_tier(ve::QualityTier::kHigh);
	CHECK(hi.ssgi_taps == 8);
	CHECK(hi.ssr_steps == 24);
	CHECK(hi.contact_steps == 12);
	CHECK(hi.ssgi);
	CHECK(hi.ssr);
	// Contact shadows were removed outright (8ae4ee4): the knob stays but every tier,
	// including High, reads as off.
	CHECK_FALSE(hi.contact_shadows);
	CHECK(hi.outlines);
	CHECK(hi.sun_shadow_map);
	CHECK(hi.glossy_sdf_rays);
	CHECK(hi.raymarched_sun_shadow);
	CHECK(ve::BeautySettings{}.ssgi_taps == hi.ssgi_taps);
}

// A tap count of a billion is a hung GPU, and a negative one is an unrolled loop that never
// terminates. The clamp is the only thing between a debug-menu typo and a driver reset.
TEST_CASE("counts are clamped into the ranges the shaders were written for") {
	ve::BeautySettings s;
	s.ssgi_taps = 9999;
	s.ssr_steps = -4;
	s.contact_steps = 1000;
	s.outline_depth_threshold = -1.0f;
	s.outline_normal_threshold = 12.0f;
	ve::clamp_settings(&s);
	CHECK(s.ssgi_taps == 16);
	CHECK(s.ssr_steps == 0);
	CHECK(s.contact_steps == 32);
	CHECK(s.outline_depth_threshold >= 0.0f);
	CHECK(s.outline_normal_threshold <= 2.0f);
}

// A zero count means the effect does no work, so it must also read as off: a pass that
// dispatches with zero taps costs a full-screen dispatch to produce nothing.
TEST_CASE("clamping to zero work also clears the enable bit") {
	ve::BeautySettings s;
	s.ssgi_taps = 0;
	s.ssr_steps = 0;
	s.contact_steps = 0;
	ve::clamp_settings(&s);
	CHECK_FALSE(s.ssgi);
	CHECK_FALSE(s.ssr);
	CHECK_FALSE(s.contact_shadows);
}

TEST_CASE("the packed flag bits are stable, because a shader hardcodes them") {
	ve::BeautySettings s = ve::settings_for_tier(ve::QualityTier::kOff);
	CHECK(ve::pack_flags(s) == 0u);
	s.ssgi = true;
	s.ssgi_taps = 8;
	CHECK((ve::pack_flags(s) & 1u) == 1u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.ssr = true;
	s.ssr_steps = 8;
	CHECK((ve::pack_flags(s) & 2u) == 2u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.contact_shadows = true;
	s.contact_steps = 8;
	CHECK((ve::pack_flags(s) & 4u) == 4u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.outlines = true;
	CHECK((ve::pack_flags(s) & 8u) == 8u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.sun_shadow_map = true;
	CHECK((ve::pack_flags(s) & 16u) == 16u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.glossy_sdf_rays = true;
	CHECK((ve::pack_flags(s) & 32u) == 32u);
	s = ve::settings_for_tier(ve::QualityTier::kOff);
	s.raymarched_sun_shadow = true;
	CHECK((ve::pack_flags(s) & 64u) == 64u);
}

TEST_CASE("an out-of-range tier falls back to High rather than to nothing") {
	const ve::BeautySettings s = ve::settings_for_tier(static_cast<ve::QualityTier>(99));
	CHECK(s.ssgi_taps == 8);
}

TEST_CASE("cost view is a flag, off in every quality tier") {
	// It is a debug view, not a quality level: switching to High must never turn the screen
	// into a heat map, and switching to Off must not be the only way to leave it.
	for (const ve::QualityTier tier : {ve::QualityTier::kOff, ve::QualityTier::kLow,
			ve::QualityTier::kMedium, ve::QualityTier::kHigh}) {
		const ve::BeautySettings s = ve::settings_for_tier(tier);
		CHECK((ve::pack_beauty_flags(s) & ve::kFlagCostView) == 0u);
	}
	ve::BeautySettings s = ve::settings_for_tier(ve::QualityTier::kHigh);
	s.cost_view = true;
	CHECK((ve::pack_beauty_flags(s) & ve::kFlagCostView) == ve::kFlagCostView);
}

namespace {

// Every field, so a tier preset that silently moves any knob fails here. A commit that adds a
// BeautySettings field adds it to this list.
void check_same(const ve::BeautySettings &got, const ve::BeautySettings &want) {
	CHECK(got.ssgi == want.ssgi);
	CHECK(got.ssr == want.ssr);
	CHECK(got.contact_shadows == want.contact_shadows);
	CHECK(got.outlines == want.outlines);
	CHECK(got.sun_shadow_map == want.sun_shadow_map);
	CHECK(got.glossy_sdf_rays == want.glossy_sdf_rays);
	CHECK(got.raymarched_sun_shadow == want.raymarched_sun_shadow);
	CHECK(got.cost_view == want.cost_view);
	CHECK(got.ssao == want.ssao);
	CHECK(got.ssgi_taps == want.ssgi_taps);
	CHECK(got.ssr_steps == want.ssr_steps);
	CHECK(got.contact_steps == want.contact_steps);
	CHECK(got.ssao_steps == want.ssao_steps);
	CHECK(got.ssao_directions == want.ssao_directions);
	CHECK(got.ssgi_radius == doctest::Approx(want.ssgi_radius));
	CHECK(got.ssgi_temporal == doctest::Approx(want.ssgi_temporal));
	CHECK(got.ssgi_strength == doctest::Approx(want.ssgi_strength));
	CHECK(got.emissive_gi_radius == doctest::Approx(want.emissive_gi_radius));
	CHECK(got.emissive_gi_strength == doctest::Approx(want.emissive_gi_strength));
	CHECK(got.outline_depth_threshold == doctest::Approx(want.outline_depth_threshold));
	CHECK(got.outline_normal_threshold == doctest::Approx(want.outline_normal_threshold));
}

} // namespace

TEST_CASE("every tier preset is pinned field by field") {
	const ve::BeautySettings high; // the struct defaults ARE High
	check_same(ve::settings_for_tier(ve::QualityTier::kHigh), high);

	ve::BeautySettings medium;
	medium.glossy_sdf_rays = false;
	medium.emissive_gi_radius = 24.0f;
	medium.ssgi_taps = 4;
	medium.ssr_steps = 12;
	medium.contact_steps = 8;
	medium.ssao_steps = 4;
	medium.ssao_directions = 4;
	check_same(ve::settings_for_tier(ve::QualityTier::kMedium), medium);

	ve::BeautySettings low;
	low.ssgi = low.ssr = low.contact_shadows = false;
	low.glossy_sdf_rays = false;
	low.ssao = false;
	low.ssgi_taps = low.ssr_steps = low.contact_steps = 0;
	low.ssao_steps = low.ssao_directions = 0;
	check_same(ve::settings_for_tier(ve::QualityTier::kLow), low);

	ve::BeautySettings off = low;
	off.outlines = off.sun_shadow_map = off.raymarched_sun_shadow = false;
	check_same(ve::settings_for_tier(ve::QualityTier::kOff), off);
}

TEST_CASE("the beauty rows satisfy the table invariants for every tier") {
	check_rows(ve::beauty_rows(),
			{ve::BeautySettings{}, ve::settings_for_tier(ve::QualityTier::kOff),
					ve::settings_for_tier(ve::QualityTier::kLow),
					ve::settings_for_tier(ve::QualityTier::kMedium),
					ve::settings_for_tier(ve::QualityTier::kHigh)});
}

TEST_CASE("the beauty store starts at High and sets counts by name") {
	ve::BeautySettingsStore store;
	check_same(store.get(), ve::settings_for_tier(ve::QualityTier::kHigh));
	CHECK(store.set_value("ssgi_taps", 4.0f));
	CHECK(store.get().ssgi_taps == 4);
	CHECK(store.set_value("ssgi_taps", 0.0f));
	CHECK_FALSE(store.get().ssgi); // normalize: zero work is off
}
