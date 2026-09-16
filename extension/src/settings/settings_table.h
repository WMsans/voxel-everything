#pragma once
// A settings table: one row per knob of a plain settings struct, naming the member, its kind,
// its hard clamp and the range a slider offers. Everything that addresses a knob by name -- the
// store, VoxelSettings, the settings panel, the config file, the inspector -- reads these rows,
// so a name, a range and a member cannot disagree. Pure: no godot-cpp.
// Spec: docs/superpowers/specs/2026-09-16-settings-store-design.md §3.1.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>

namespace ve {

enum class SettingKind : uint8_t { kBool, kInt, kFloat, kColor, kEnum };

// bool, int, float and enum index in v[0]; colour in v[0..2].
struct SettingValue {
	SettingKind kind = SettingKind::kFloat;
	float v[3] = {0.0f, 0.0f, 0.0f};

	static SettingValue of_bool(bool b) { return {SettingKind::kBool, {b ? 1.0f : 0.0f, 0.0f, 0.0f}}; }
	static SettingValue of_int(int i) { return {SettingKind::kInt, {static_cast<float>(i), 0.0f, 0.0f}}; }
	static SettingValue of_enum(int i) { return {SettingKind::kEnum, {static_cast<float>(i), 0.0f, 0.0f}}; }
	static SettingValue of_float(float f) { return {SettingKind::kFloat, {f, 0.0f, 0.0f}}; }
	static SettingValue of_color(float r, float g, float b) { return {SettingKind::kColor, {r, g, b}}; }
};

// Exactly one member pointer is set, matching `kind` (kInt and kEnum use `i`). min/max is the
// hard clamp -- per channel for colour, ignored for bool -- and ui_min/ui_max/step the slider.
template <class T>
struct SettingRow {
	const char *name = nullptr;
	const char *label = nullptr;
	SettingKind kind = SettingKind::kFloat;
	bool T::*b = nullptr;
	int T::*i = nullptr;
	float T::*f = nullptr;
	float (T::*c)[3] = nullptr;
	float min = 0.0f;
	float max = 0.0f;
	float ui_min = 0.0f;
	float ui_max = 0.0f;
	float step = 0.0f;
	std::span<const char *const> options = {};
	const char *hint = nullptr;
};

template <class T>
constexpr SettingRow<T> bool_row(const char *name, const char *label, bool T::*m,
		const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kBool;
	r.b = m;
	r.max = r.ui_max = r.step = 1.0f;
	r.hint = hint;
	return r;
}

template <class T>
constexpr SettingRow<T> int_row(const char *name, const char *label, int T::*m, int min, int max,
		int ui_min, int ui_max, int step = 1, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kInt;
	r.i = m;
	r.min = static_cast<float>(min);
	r.max = static_cast<float>(max);
	r.ui_min = static_cast<float>(ui_min);
	r.ui_max = static_cast<float>(ui_max);
	r.step = static_cast<float>(step);
	r.hint = hint;
	return r;
}

template <class T>
constexpr SettingRow<T> float_row(const char *name, const char *label, float T::*m, float min,
		float max, float ui_min, float ui_max, float step, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kFloat;
	r.f = m;
	r.min = min;
	r.max = max;
	r.ui_min = ui_min;
	r.ui_max = ui_max;
	r.step = step;
	r.hint = hint;
	return r;
}

template <class T>
constexpr SettingRow<T> color_row(const char *name, const char *label, float (T::*m)[3], float min,
		float max, float ui_min, float ui_max, float step, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kColor;
	r.c = m;
	r.min = min;
	r.max = max;
	r.ui_min = ui_min;
	r.ui_max = ui_max;
	r.step = step;
	r.hint = hint;
	return r;
}

// Index into `options`. `min` may be negative for an enum with an out-of-table state (display
// resolution uses -1 for "not a preset size").
template <class T>
constexpr SettingRow<T> enum_row(const char *name, const char *label, int T::*m,
		std::span<const char *const> options, int min = 0, const char *hint = nullptr) {
	SettingRow<T> r;
	r.name = name;
	r.label = label;
	r.kind = SettingKind::kEnum;
	r.i = m;
	r.options = options;
	r.min = r.ui_min = static_cast<float>(min);
	r.max = r.ui_max = static_cast<float>(options.size()) - 1.0f;
	r.step = 1.0f;
	r.hint = hint;
	return r;
}

// NaN floors to lo; +-inf clamp to the nearer bound (spec decision 3).
inline float clamp_setting(float v, float lo, float hi) {
	if (!(v > lo)) return lo;
	if (!(v < hi)) return hi;
	return v;
}

template <class T>
const SettingRow<T> *find_row(std::span<const SettingRow<T>> rows, const char *name) {
	if (!name) return nullptr;
	for (const SettingRow<T> &r : rows)
		if (std::strcmp(r.name, name) == 0) return &r;
	return nullptr;
}

template <class T>
SettingValue read(const SettingRow<T> &r, const T &s) {
	SettingValue v;
	v.kind = r.kind;
	switch (r.kind) {
		case SettingKind::kBool: v.v[0] = (s.*r.b) ? 1.0f : 0.0f; break;
		case SettingKind::kInt:
		case SettingKind::kEnum: v.v[0] = static_cast<float>(s.*r.i); break;
		case SettingKind::kFloat: v.v[0] = s.*r.f; break;
		case SettingKind::kColor:
			for (int k = 0; k < 3; k++) v.v[k] = (s.*r.c)[k];
			break;
	}
	return v;
}

// Clamps into the row's hard range (ints and enums round after clamping). False, and the struct
// untouched, when the value's kind is not the row's kind.
template <class T>
bool write(const SettingRow<T> &r, T *s, const SettingValue &v) {
	if (!s || v.kind != r.kind) return false;
	switch (r.kind) {
		case SettingKind::kBool: s->*r.b = !std::isnan(v.v[0]) && v.v[0] != 0.0f; break;
		case SettingKind::kInt:
		case SettingKind::kEnum:
			s->*r.i = static_cast<int>(std::lround(clamp_setting(v.v[0], r.min, r.max)));
			break;
		case SettingKind::kFloat: s->*r.f = clamp_setting(v.v[0], r.min, r.max); break;
		case SettingKind::kColor:
			for (int k = 0; k < 3; k++) (s->*r.c)[k] = clamp_setting(v.v[k], r.min, r.max);
			break;
	}
	return true;
}

template <class T>
void clamp_all(std::span<const SettingRow<T>> rows, T *s) {
	if (!s) return;
	for (const SettingRow<T> &r : rows) write(r, s, read(r, *s));
}

// A row without its member pointer, for callers that do not know T.
struct RowInfo {
	const char *name = nullptr;
	const char *label = nullptr;
	const char *hint = nullptr;
	SettingKind kind = SettingKind::kFloat;
	float min = 0.0f;
	float max = 0.0f;
	float ui_min = 0.0f;
	float ui_max = 0.0f;
	float step = 0.0f;
	std::span<const char *const> options = {};
};

template <class T>
RowInfo row_info(const SettingRow<T> &r) {
	RowInfo info;
	info.name = r.name;
	info.label = r.label;
	info.hint = r.hint;
	info.kind = r.kind;
	info.min = r.min;
	info.max = r.max;
	info.ui_min = r.ui_min;
	info.ui_max = r.ui_max;
	info.step = r.step;
	info.options = r.options;
	return info;
}

} // namespace ve
