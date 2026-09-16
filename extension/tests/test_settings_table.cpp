#include <doctest/doctest.h>
#include "settings/settings_store.h"
#include "settings_row_checks.h"
#include <cstring>
#include <limits>

namespace {

struct Knobs {
	bool on = true;
	int count = 4;
	float gain = 1.0f;
	float tint[3] = {0.5f, 0.5f, 0.5f};
	int mode = 1;
};

const char *const kModes[] = {"A", "B", "C"};

const ve::SettingRow<Knobs> kKnobRows[] = {
	ve::bool_row("on", "On", &Knobs::on),
	ve::int_row("count", "Count", &Knobs::count, 0, 8, 0, 8),
	ve::float_row("gain", "Gain", &Knobs::gain, 0.0f, 4.0f, 0.0f, 2.0f, 0.1f),
	ve::color_row("tint", "Tint", &Knobs::tint, 0.0f, 1.0f, 0.0f, 1.0f, 0.01f),
	ve::enum_row("mode", "Mode", &Knobs::mode, std::span<const char *const>(kModes)),
};

std::span<const ve::SettingRow<Knobs>> knob_rows() {
	return kKnobRows;
}

// A rule that spans fields, like BeautySettings' "zero taps is off".
void zero_count_is_off(Knobs *k) {
	if (k->count == 0) k->on = false;
}

using Store = ve::SettingsStore<Knobs>;
using V = ve::SettingValue;

const float kNaN = std::numeric_limits<float>::quiet_NaN();
const float kInf = std::numeric_limits<float>::infinity();

} // namespace

TEST_CASE("the test table satisfies the row invariants") {
	check_rows(knob_rows(), {Knobs{}});
}

TEST_CASE("a row reads and writes its own member for every kind") {
	Knobs k;
	const auto rows = knob_rows();
	CHECK(ve::write(rows[0], &k, V::of_bool(false)));
	CHECK(ve::write(rows[1], &k, V::of_int(6)));
	CHECK(ve::write(rows[2], &k, V::of_float(1.5f)));
	CHECK(ve::write(rows[3], &k, V::of_color(0.1f, 0.2f, 0.3f)));
	CHECK(ve::write(rows[4], &k, V::of_enum(2)));
	CHECK_FALSE(k.on);
	CHECK(k.count == 6);
	CHECK(k.gain == doctest::Approx(1.5f));
	CHECK(k.tint[0] == doctest::Approx(0.1f));
	CHECK(k.tint[1] == doctest::Approx(0.2f));
	CHECK(k.tint[2] == doctest::Approx(0.3f));
	CHECK(k.mode == 2);
	CHECK(ve::read(rows[1], k).kind == ve::SettingKind::kInt);
	CHECK(ve::read(rows[1], k).v[0] == doctest::Approx(6.0f));
	CHECK(ve::read(rows[3], k).v[2] == doctest::Approx(0.3f));
	CHECK(ve::read(rows[4], k).kind == ve::SettingKind::kEnum);
}

TEST_CASE("writes clamp to the hard range; NaN floors and infinities clamp to the nearer bound") {
	Knobs k;
	const auto rows = knob_rows();
	ve::write(rows[2], &k, V::of_float(9.0f));
	CHECK(k.gain == 4.0f);
	ve::write(rows[2], &k, V::of_float(-1.0f));
	CHECK(k.gain == 0.0f);
	ve::write(rows[2], &k, V::of_float(kInf));
	CHECK(k.gain == 4.0f);
	ve::write(rows[2], &k, V::of_float(kNaN));
	CHECK(k.gain == 0.0f);
	ve::write(rows[2], &k, V::of_float(-kInf));
	CHECK(k.gain == 0.0f);
	ve::write(rows[1], &k, V::of_int(99));
	CHECK(k.count == 8);
	ve::write(rows[1], &k, V{ve::SettingKind::kInt, {kNaN, 0.0f, 0.0f}});
	CHECK(k.count == 0);
	ve::write(rows[3], &k, V::of_color(kNaN, 2.0f, -1.0f));
	CHECK(k.tint[0] == 0.0f);
	CHECK(k.tint[1] == 1.0f);
	CHECK(k.tint[2] == 0.0f);
	ve::write(rows[0], &k, V{ve::SettingKind::kBool, {kNaN, 0.0f, 0.0f}});
	CHECK_FALSE(k.on);
	ve::write(rows[4], &k, V::of_enum(7));
	CHECK(k.mode == 2);
}

TEST_CASE("ints and enums round to nearest after clamping") {
	Knobs k;
	const auto rows = knob_rows();
	ve::write(rows[1], &k, V{ve::SettingKind::kInt, {2.6f, 0.0f, 0.0f}});
	CHECK(k.count == 3);
	ve::write(rows[4], &k, V{ve::SettingKind::kEnum, {0.4f, 0.0f, 0.0f}});
	CHECK(k.mode == 0);
}

TEST_CASE("a value of the wrong kind is refused and leaves the struct untouched") {
	Knobs k;
	const auto rows = knob_rows();
	CHECK_FALSE(ve::write(rows[2], &k, V::of_int(3)));
	CHECK(k.gain == 1.0f);
	CHECK_FALSE(ve::write(rows[0], &k, V::of_float(0.0f)));
	CHECK(k.on);
}

TEST_CASE("rows are found by name and an unknown or null name finds none") {
	CHECK(ve::find_row(knob_rows(), "gain") == &kKnobRows[2]);
	CHECK(ve::find_row(knob_rows(), "nope") == nullptr);
	CHECK(ve::find_row(knob_rows(), nullptr) == nullptr);
}

TEST_CASE("clamp_all pulls every member into range") {
	Knobs k;
	k.count = -3;
	k.gain = kNaN;
	k.tint[1] = 5.0f;
	k.mode = 9;
	ve::clamp_all(knob_rows(), &k);
	CHECK(k.count == 0);
	CHECK(k.gain == 0.0f);
	CHECK(k.tint[1] == 1.0f);
	CHECK(k.mode == 2);
}

TEST_CASE("the store resolves base, then overrides, then normalize") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK(store.get().on);
	CHECK(store.set("count", V::of_int(0)));
	CHECK_FALSE(store.get().on);
	CHECK(store.get().count == 0);
	// A pure function of (base, overrides): nothing the normalize step did is latched.
	CHECK(store.set("count", V::of_int(3)));
	CHECK(store.get().on);
}

TEST_CASE("rebase keeps overrides and set(T) drops them") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK(store.set("gain", V::of_float(1.5f)));
	Knobs other;
	other.gain = 3.0f;
	other.count = 7;
	store.rebase(other);
	CHECK(store.get().gain == doctest::Approx(1.5f));
	CHECK(store.get().count == 7);
	CHECK(store.base().gain == doctest::Approx(3.0f));
	store.set(other);
	CHECK(store.get().gain == doctest::Approx(3.0f));
	CHECK(store.overrides().empty());
}

TEST_CASE("clear and clear_all drop overrides; an unknown name reports false") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	store.set("gain", V::of_float(1.5f));
	store.set("count", V::of_int(2));
	CHECK(store.clear("gain"));
	CHECK(store.get().gain == doctest::Approx(1.0f));
	CHECK(store.get().count == 2);
	CHECK_FALSE(store.clear("nope"));
	store.clear_all();
	CHECK(store.get().count == 4);
	CHECK(store.overrides().empty());
}

TEST_CASE("overrides report the clamped value that was set, in row order") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	store.set("count", V::of_int(0));
	store.set("on", V::of_bool(true));
	store.set("gain", V::of_float(9.0f));
	const auto o = store.overrides();
	REQUIRE(o.size() == 3);
	CHECK(std::strcmp(o[0].first, "on") == 0);
	// normalize turned the resolved value off; the override is still what was set.
	CHECK(o[0].second.v[0] == 1.0f);
	CHECK_FALSE(store.get().on);
	CHECK(std::strcmp(o[1].first, "count") == 0);
	CHECK(std::strcmp(o[2].first, "gain") == 0);
	CHECK(o[2].second.v[0] == doctest::Approx(4.0f));
}

TEST_CASE("the store refuses unknown names and wrong kinds without recording an override") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK_FALSE(store.set("nope", V::of_float(1.0f)));
	CHECK_FALSE(store.set("gain", V::of_int(1)));
	CHECK(store.overrides().empty());
}

TEST_CASE("set_value addresses bool, int, float and enum rows and refuses colour") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	CHECK(store.set_value("on", 0.0f));
	CHECK_FALSE(store.get().on);
	CHECK(store.set_value("count", 5.0f));
	CHECK(store.get().count == 5);
	CHECK(store.set_value("gain", 2.0f));
	CHECK(store.value("gain") == doctest::Approx(2.0f));
	CHECK(store.set_value("mode", 0.0f));
	CHECK(store.get().mode == 0);
	CHECK_FALSE(store.set_value("tint", 0.2f));
	CHECK(store.value("nope") == 0.0f);
}

TEST_CASE("get_default is the normalized base and ignores overrides") {
	Knobs base;
	base.count = 0;
	Store store(knob_rows(), zero_count_is_off, base);
	store.set("on", V::of_bool(true));
	store.set("gain", V::of_float(2.0f));
	V v;
	CHECK(store.get_default("gain", &v));
	CHECK(v.v[0] == doctest::Approx(1.0f));
	CHECK(store.get_default("on", &v));
	CHECK(v.v[0] == 0.0f);
	CHECK_FALSE(store.get_default("nope", &v));
}

TEST_CASE("the listener sees each resolved value and runs outside the lock") {
	struct Seen {
		Store *store = nullptr;
		int calls = 0;
		float gain = 0.0f;
	};
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	Seen seen;
	seen.store = &store;
	store.set_listener([](const Knobs &k, void *ctx) {
		auto *s = static_cast<Seen *>(ctx);
		s->calls++;
		s->gain = k.gain;
		// Re-entering the store deadlocks if the listener runs under its lock.
		CHECK(s->store->get().gain == doctest::Approx(k.gain));
	}, &seen);
	store.set("gain", V::of_float(2.5f));
	store.rebase(Knobs{});
	store.clear_all();
	CHECK(seen.calls == 3);
	CHECK(seen.gain == doctest::Approx(1.0f));
}

TEST_CASE("rows() describes every row for type-erased callers") {
	Store store(knob_rows(), zero_count_is_off, Knobs{});
	const ve::SettingsGroup &g = store;
	REQUIRE(g.rows().size() == 5);
	CHECK(std::strcmp(g.rows()[4].name, "mode") == 0);
	CHECK(g.rows()[4].kind == ve::SettingKind::kEnum);
	CHECK(g.rows()[4].options.size() == 3);
	CHECK(g.rows()[2].ui_max == doctest::Approx(2.0f));
}
