#pragma once
// SettingValue <-> Variant, for the three places GDScript meets a settings row:
// VoxelSettings, its inspector properties and debug_beauty_settings.
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include "settings/settings_table.h"

namespace godot {

// bool, int (int and enum), float, or Color.
Variant setting_to_variant(const ve::SettingValue &v);
// Reads `in` as `kind`. A bool, int or float coerces to any non-colour kind; only a Color reads as
// a colour. False when the Variant cannot be read as the kind.
bool setting_from_variant(ve::SettingKind kind, const Variant &in, ve::SettingValue *out);
// "bool", "int", "float", "color" or "enum".
String setting_kind_name(ve::SettingKind kind);

} // namespace godot
