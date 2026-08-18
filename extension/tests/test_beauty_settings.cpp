#include <doctest/doctest.h>
#include "shade/beauty_settings.h"

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
	CHECK(hi.contact_shadows);
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
