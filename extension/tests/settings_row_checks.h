#pragma once
// Invariants every settings table holds (spec 2026-09-16 decision 5): names unique, one member
// per row and no member twice, min <= ui_min <= ui_max <= max, a positive step, an enum's max
// matching its options, and every preset a store can be based on lying inside the slider range.
// The last one is what makes a menu range that excludes a shipped value (S5) unrepresentable.
#include <doctest/doctest.h>
#include <cstring>
#include <initializer_list>
#include <span>
#include "settings/settings_table.h"

template <class T>
void check_rows(std::span<const ve::SettingRow<T>> rows, std::initializer_list<T> presets) {
	REQUIRE(!rows.empty());
	for (size_t i = 0; i < rows.size(); i++) {
		const ve::SettingRow<T> &r = rows[i];
		REQUIRE(r.name != nullptr);
		CAPTURE(r.name);
		CHECK(r.label != nullptr);
		CHECK(r.min <= r.ui_min);
		CHECK(r.ui_min <= r.ui_max);
		CHECK(r.ui_max <= r.max);
		CHECK(r.step > 0.0f);
		const int members = (r.b != nullptr) + (r.i != nullptr) + (r.f != nullptr) + (r.c != nullptr);
		CHECK(members == 1);
		switch (r.kind) {
			case ve::SettingKind::kBool: CHECK(r.b != nullptr); break;
			case ve::SettingKind::kInt:
				CHECK(r.i != nullptr);
				CHECK(r.options.empty());
				break;
			case ve::SettingKind::kEnum:
				CHECK(r.i != nullptr);
				REQUIRE(!r.options.empty());
				CHECK(r.max == static_cast<float>(r.options.size() - 1));
				break;
			case ve::SettingKind::kFloat: CHECK(r.f != nullptr); break;
			case ve::SettingKind::kColor: CHECK(r.c != nullptr); break;
		}
		for (size_t j = i + 1; j < rows.size(); j++) {
			const ve::SettingRow<T> &o = rows[j];
			CHECK(std::strcmp(r.name, o.name) != 0);
			if (r.b) CHECK(r.b != o.b);
			if (r.i) CHECK(r.i != o.i);
			if (r.f) CHECK(r.f != o.f);
			if (r.c) CHECK(r.c != o.c);
		}
	}
	for (const T &preset : presets) {
		for (const ve::SettingRow<T> &r : rows) {
			if (r.kind == ve::SettingKind::kBool) continue;
			CAPTURE(r.name);
			const ve::SettingValue v = ve::read(r, preset);
			const int channels = r.kind == ve::SettingKind::kColor ? 3 : 1;
			for (int k = 0; k < channels; k++) {
				CHECK(v.v[k] >= r.ui_min);
				CHECK(v.v[k] <= r.ui_max);
			}
		}
	}
}
