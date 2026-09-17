#include "settings/godot/setting_variant.h"
#include <godot_cpp/variant/color.hpp>
#include <cmath>

namespace godot {

Variant setting_to_variant(const ve::SettingValue &v) {
	switch (v.kind) {
		case ve::SettingKind::kBool: return v.v[0] != 0.0f;
		case ve::SettingKind::kInt:
		case ve::SettingKind::kEnum: return static_cast<int64_t>(std::lround(v.v[0]));
		case ve::SettingKind::kFloat: return v.v[0];
		case ve::SettingKind::kColor: return Color(v.v[0], v.v[1], v.v[2]);
	}
	return Variant();
}

bool setting_from_variant(ve::SettingKind kind, const Variant &in, ve::SettingValue *out) {
	if (!out) return false;
	out->kind = kind;
	if (kind == ve::SettingKind::kColor) {
		if (in.get_type() != Variant::COLOR) return false;
		const Color c = in;
		out->v[0] = c.r;
		out->v[1] = c.g;
		out->v[2] = c.b;
		return true;
	}
	switch (in.get_type()) {
		case Variant::BOOL: out->v[0] = static_cast<bool>(in) ? 1.0f : 0.0f; return true;
		case Variant::INT: out->v[0] = static_cast<float>(static_cast<int64_t>(in)); return true;
		case Variant::FLOAT: out->v[0] = static_cast<float>(static_cast<double>(in)); return true;
		default: return false;
	}
}

String setting_kind_name(ve::SettingKind kind) {
	switch (kind) {
		case ve::SettingKind::kBool: return "bool";
		case ve::SettingKind::kInt: return "int";
		case ve::SettingKind::kFloat: return "float";
		case ve::SettingKind::kColor: return "color";
		case ve::SettingKind::kEnum: return "enum";
	}
	return String();
}

} // namespace godot
