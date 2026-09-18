#include <doctest/doctest.h>
#include "terrain/stage_library.h"

namespace {
VE_STAGE_SLOTS(Tws, p, sdf);
VE_STAGE_PARAMS(Tws, off);

void test_writes_sdf(ve::FieldCtx &ctx, const TwsSlots &s, const TwsParams &p,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.v(s.p)[1] - p.off;
}
} // namespace

VE_REGISTER_STAGE("ve::test_writes_sdf", Tws, test_writes_sdf);

TEST_CASE("a registered stage is found by symbol and runs") {
	const ve::StageBinding *b = ve::StageLibrary::instance().lookup("ve::test_writes_sdf");
	REQUIRE(b != nullptr);
	REQUIRE(b->fn != nullptr);

	ve::FieldCtx ctx;
	const int slots[2] = {0, 1};  // p, sdf in TwsSlots declaration order
	ctx.v(slots[0])[1] = 10.0f;
	const float params[1] = {4.0f};
	b->fn(ctx, slots, params, ve::FieldResources{});
	CHECK(ctx.f(slots[1]) == doctest::Approx(6.0f));
}

TEST_CASE("an unknown symbol resolves to null rather than crashing") {
	CHECK(ve::StageLibrary::instance().lookup("ve::nope") == nullptr);
}

TEST_CASE("split_binding_names trims and drops empties") {
	CHECK(ve::split_binding_names("").empty());
	CHECK(ve::split_binding_names("p, sdf, height") ==
			std::vector<std::string>{"p", "sdf", "height"});
}
