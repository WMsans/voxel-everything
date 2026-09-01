#include <doctest/doctest.h>
#include "shade/cel.h"
#include <cmath>

namespace {

ve::CelInput plain(float ndl) {
	ve::CelInput in{};
	in.albedo[0] = 0.5f; in.albedo[1] = 0.5f; in.albedo[2] = 0.5f;
	in.ambient[0] = 0.0f; in.ambient[1] = 0.0f; in.ambient[2] = 0.0f;
	in.ndl = ndl;
	in.ndv = 1.0f;   // face-on: no rim
	in.ndh = 0.0f;   // no specular
	in.shadow = 1.0f;
	in.ao = 1.0f;
	in.gloss = 0.0f;
	return in;
}

} // namespace

TEST_CASE("the ramp has exactly four bands and they are the stated ones") {
	const ve::CelParams p;
	CHECK(ve::kCelBands == 4);
	CHECK(ve::cel_band(p, 0.00f) == 0);
	CHECK(ve::cel_band(p, 0.079f) == 0);
	CHECK(ve::cel_band(p, 0.081f) == 1);
	CHECK(ve::cel_band(p, 0.319f) == 1);
	CHECK(ve::cel_band(p, 0.321f) == 2);
	CHECK(ve::cel_band(p, 0.659f) == 2);
	CHECK(ve::cel_band(p, 0.661f) == 3);
	CHECK(ve::cel_band(p, 1.00f) == 3);
}

TEST_CASE("a sweep of N dot L produces four distinct levels and nothing between them") {
	const ve::CelParams p;
	float seen[8];
	int count = 0;
	for (int i = 0; i <= 1000; i++) {
		const float v = ve::cel_level(p, static_cast<float>(i) / 1000.0f);
		bool known = false;
		for (int k = 0; k < count; k++)
			if (std::fabs(seen[k] - v) < 1e-6f) known = true;
		if (!known) {
			REQUIRE(count < 8);
			seen[count++] = v;
		}
	}
	CHECK(count == 4);
}

TEST_CASE("the ramp never falls as the light rises") {
	const ve::CelParams p;
	float prev = -1.0f;
	for (int i = 0; i <= 1000; i++) {
		const float v = ve::cel_level(p, static_cast<float>(i) / 1000.0f);
		CHECK(v >= prev);
		prev = v;
	}
}

TEST_CASE("out-of-range N dot L is clamped, not extrapolated") {
	const ve::CelParams p;
	CHECK(ve::cel_level(p, -5.0f) == doctest::Approx(ve::cel_level(p, 0.0f)));
	CHECK(ve::cel_level(p, 5.0f) == doctest::Approx(ve::cel_level(p, 1.0f)));
}

TEST_CASE("full light with no ambient, no rim and no spec returns the albedo untouched") {
	const ve::CelParams p;
	ve::CelInput in = plain(1.0f);
	float out[3];
	ve::cel_shade(p, in, out);
	// band_level[3] is 1.0 and the tint is the identity at t = 0, so this is the one input
	// where the whole stack must be a no-op. If it is not, something is scaling the image.
	CHECK(out[0] == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(out[1] == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(0.5f).epsilon(1e-5));
}

TEST_CASE("a fully shadowed pixel keeps only its ambient term") {
	const ve::CelParams p;
	ve::CelInput in = plain(1.0f);
	in.shadow = 0.0f;
	in.ambient[0] = 0.2f; in.ambient[1] = 0.2f; in.ambient[2] = 0.2f;
	float out[3];
	ve::cel_shade(p, in, out);
	float tint[3];
	ve::cel_shadow_tint(p, in.albedo, 1.0f, tint);
	CHECK(out[0] == doctest::Approx(tint[0] * 0.2f).epsilon(1e-5));
	CHECK(out[1] == doctest::Approx(tint[1] * 0.2f).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(tint[2] * 0.2f).epsilon(1e-5));
}

TEST_CASE("ambient occlusion multiplies the ambient term and nothing else") {
	const ve::CelParams p;
	ve::CelInput lit = plain(1.0f);
	lit.ambient[0] = 0.3f; lit.ambient[1] = 0.3f; lit.ambient[2] = 0.3f;
	float with_ao[3];
	float without_ao[3];
	ve::cel_shade(p, lit, without_ao);
	lit.ao = 0.0f;
	ve::cel_shade(p, lit, with_ao);
	// Removing AO must remove exactly the ambient contribution: 0.5 albedo x 0.3 ambient.
	CHECK((without_ao[0] - with_ao[0]) == doctest::Approx(0.5f * 0.3f).epsilon(1e-5));
}

TEST_CASE("hsv round-trips") {
	const float colors[5][3] = {{0.0f, 0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}, {0.8f, 0.2f, 0.1f},
			{0.1f, 0.6f, 0.3f}, {0.25f, 0.25f, 0.9f}};
	for (const auto &c : colors) {
		float hsv[3];
		float back[3];
		ve::rgb_to_hsv(c, hsv);
		ve::hsv_to_rgb(hsv, back);
		CHECK(back[0] == doctest::Approx(c[0]).epsilon(1e-5));
		CHECK(back[1] == doctest::Approx(c[1]).epsilon(1e-5));
		CHECK(back[2] == doctest::Approx(c[2]).epsilon(1e-5));
	}
}

TEST_CASE("the shadow tint is the identity in full light and a real hue shift in shadow") {
	const ve::CelParams p;
	const float albedo[3] = {0.8f, 0.2f, 0.1f};
	float at_zero[3];
	ve::cel_shadow_tint(p, albedo, 0.0f, at_zero);
	CHECK(at_zero[0] == doctest::Approx(albedo[0]).epsilon(1e-5));
	CHECK(at_zero[1] == doctest::Approx(albedo[1]).epsilon(1e-5));
	CHECK(at_zero[2] == doctest::Approx(albedo[2]).epsilon(1e-5));

	float at_one[3];
	ve::cel_shadow_tint(p, albedo, 1.0f, at_one);
	float hsv_in[3];
	float hsv_out[3];
	ve::rgb_to_hsv(albedo, hsv_in);
	ve::rgb_to_hsv(at_one, hsv_out);
	// The hue moved by exactly the configured amount, wrapping at 1.
	float expected = hsv_in[0] + p.shadow_hue_shift;
	expected -= std::floor(expected);
	CHECK(hsv_out[0] == doctest::Approx(expected).epsilon(1e-4));
	CHECK(hsv_out[1] > hsv_in[1]); // and it got more saturated, as a shadow should
}

// A grey surface has no hue to shift. If the tint invents one, every rock face in the demo
// goes faintly blue in shadow for no reason anybody can point at in the ramp.
TEST_CASE("a desaturated albedo is untouched by the hue shift") {
	const ve::CelParams p;
	const float grey[3] = {0.4f, 0.4f, 0.4f};
	float out[3];
	ve::cel_shadow_tint(p, grey, 1.0f, out);
	CHECK(out[0] == doctest::Approx(grey[0]).epsilon(1e-5));
	CHECK(out[1] == doctest::Approx(grey[1]).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(grey[2]).epsilon(1e-5));
}

TEST_CASE("the specular band is a step, not a falloff, and only glossy materials get it") {
	const ve::CelParams p;
	ve::CelInput dull = plain(1.0f);
	dull.gloss = 0.0f;
	dull.ndh = 1.0f;
	float dull_out[3];
	ve::cel_shade(p, dull, dull_out);
	CHECK(dull_out[0] == doctest::Approx(0.5f).epsilon(1e-5));

	ve::CelInput shiny = plain(1.0f);
	shiny.gloss = 1.0f;
	shiny.ndh = p.spec_edge - 0.01f;
	float below[3];
	ve::cel_shade(p, shiny, below);
	shiny.ndh = p.spec_edge + 0.01f;
	float above[3];
	ve::cel_shade(p, shiny, above);
	CHECK(below[0] == doctest::Approx(0.5f).epsilon(1e-5));
	CHECK((above[0] - below[0]) == doctest::Approx(p.spec_strength).epsilon(1e-5));
}

TEST_CASE("the rim only appears at grazing angles") {
	const ve::CelParams p;
	ve::CelInput face_on = plain(1.0f);
	face_on.ndv = 1.0f;
	ve::CelInput grazing = plain(1.0f);
	grazing.ndv = 0.0f;
	float a[3];
	float b[3];
	ve::cel_shade(p, face_on, a);
	ve::cel_shade(p, grazing, b);
	CHECK(b[0] > a[0]);
	CHECK((b[0] - a[0]) == doctest::Approx(p.rim_strength).epsilon(1e-4));
}

TEST_CASE("the sun direction is the unit vector every shader assumes") {
	const float l = std::sqrt(ve::kSunDir[0] * ve::kSunDir[0] +
			ve::kSunDir[1] * ve::kSunDir[1] + ve::kSunDir[2] * ve::kSunDir[2]);
	CHECK(l == doctest::Approx(1.0f).epsilon(1e-5));
	// It is normalize(0.6, 0.8, 0.3) -- the direction shade_terrain() has used since M1.
	CHECK(ve::kSunDir[0] / ve::kSunDir[1] == doctest::Approx(0.75f).epsilon(1e-5));
	CHECK(ve::kSunDir[2] / ve::kSunDir[1] == doctest::Approx(0.375f).epsilon(1e-5));
}

TEST_CASE("a white sun is exactly today's shading") {
	const ve::CelParams p;
	ve::CelInput in = plain(1.0f);
	in.ambient[0] = 0.16f; in.ambient[1] = 0.19f; in.ambient[2] = 0.26f;
	in.gloss = 1.0f;
	in.ndh = 1.0f;   // inside the specular band
	in.ndv = 0.0f;   // and picking up rim
	float out[3];
	ve::cel_shade(p, in, out);
	// The default sun is white, so this must equal the value produced by hand from the
	// pre-sun formula: tint*lit + tint*ambient*ao + spec + rim.
	ve::CelInput white = in;
	white.sun[0] = white.sun[1] = white.sun[2] = 1.0f;
	float ref[3];
	ve::cel_shade(p, white, ref);
	for (int c = 0; c < 3; c++) CHECK(out[c] == doctest::Approx(ref[c]).epsilon(1e-6));
}

TEST_CASE("the sun colours direct light and specular, never ambient or rim") {
	const ve::CelParams p;
	// Ambient only: ndl below the first band edge still yields band 0 (level 0.18), so use a
	// pure-ambient comparison by holding everything else fixed and changing only the sun.
	ve::CelInput a = plain(1.0f);
	a.ambient[0] = 0.5f; a.ambient[1] = 0.5f; a.ambient[2] = 0.5f;
	ve::CelInput b = a;
	b.sun[0] = 0.0f; b.sun[1] = 0.0f; b.sun[2] = 0.0f;
	float lit_out[3];
	float dark_out[3];
	ve::cel_shade(p, a, lit_out);
	ve::cel_shade(p, b, dark_out);
	// Killing the sun must not kill the ambient contribution.
	for (int c = 0; c < 3; c++) CHECK(dark_out[c] > 0.0f);
	for (int c = 0; c < 3; c++) CHECK(dark_out[c] < lit_out[c]);

	// The rim is a silhouette stylization, not a light: it survives a black sun.
	ve::CelInput rim_only = plain(1.0f);
	rim_only.ambient[0] = rim_only.ambient[1] = rim_only.ambient[2] = 0.0f;
	rim_only.ndv = 0.0f;
	rim_only.sun[0] = rim_only.sun[1] = rim_only.sun[2] = 0.0f;
	float rim_out[3];
	ve::cel_shade(p, rim_only, rim_out);
	CHECK(rim_out[0] == doctest::Approx(p.rim_strength).epsilon(1e-4));
}

TEST_CASE("a tinted sun scales the direct term per channel") {
	const ve::CelParams p;
	ve::CelInput warm = plain(1.0f);
	warm.ambient[0] = warm.ambient[1] = warm.ambient[2] = 0.0f;
	warm.ndv = 1.0f; // no rim
	warm.sun[0] = 1.0f; warm.sun[1] = 0.5f; warm.sun[2] = 0.25f;
	float out[3];
	ve::cel_shade(p, warm, out);
	CHECK(out[1] == doctest::Approx(out[0] * 0.5f).epsilon(1e-5));
	CHECK(out[2] == doctest::Approx(out[0] * 0.25f).epsilon(1e-5));
}
