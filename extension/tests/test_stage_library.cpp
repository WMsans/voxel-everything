#include <doctest/doctest.h>
#include "terrain/stage_library.h"

namespace {
void test_writes_sdf(ve::FieldCtx &ctx, const ve::StageSlots &s, const ve::StageParams &p,
		const ve::FieldResources &) {
	ctx.f(s.sdf) = ctx.v(s.p)[1] - p.at(0);
}
} // namespace

VE_REGISTER_STAGE("ve::test_writes_sdf", test_writes_sdf);

TEST_CASE("a registered stage is found by symbol and runs") {
	ve::StageFn fn = ve::StageLibrary::instance().lookup("ve::test_writes_sdf");
	REQUIRE(fn != nullptr);

	ve::FieldCtx ctx;
	ve::StageSlots slots;
	ctx.v(slots.p)[1] = 10.0f;
	const float params[1] = {4.0f};
	ve::StageParams sp{params, 1};
	fn(ctx, slots, sp, ve::FieldResources{});
	CHECK(ctx.f(slots.sdf) == doctest::Approx(6.0f));
}

TEST_CASE("an unknown symbol resolves to null rather than crashing") {
	CHECK(ve::StageLibrary::instance().lookup("ve::nope") == nullptr);
}

TEST_CASE("StageParams::at is bounds-safe") {
	ve::StageParams empty;
	CHECK(empty.at(0) == doctest::Approx(0.0f));
	const float v[1] = {3.0f};
	ve::StageParams one{v, 1};
	CHECK(one.at(0) == doctest::Approx(3.0f));
	CHECK(one.at(1) == doctest::Approx(0.0f));
	CHECK(one.at(-1) == doctest::Approx(0.0f));
}
