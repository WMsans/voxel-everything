#include "grass/grass_settings_store.h"
#include <cstring>

namespace ve {
namespace {
// One table, two directions: a name that can be set can be read, and neither list can drift
// from the other.
struct FloatField { const char *name; float GrassSettings::*member; };
const FloatField kFloatFields[] = {
	{"reach_m", &GrassSettings::reach_m},
	{"vertical_reach_m", &GrassSettings::vertical_reach_m},
	{"blade_width_m", &GrassSettings::blade_width_m},
	{"blade_height_m", &GrassSettings::blade_height_m},
	{"height_jitter", &GrassSettings::height_jitter},
	{"slope_cos_min", &GrassSettings::slope_cos_min},
	{"wind_strength", &GrassSettings::wind_strength},
	{"wind_speed", &GrassSettings::wind_speed},
	{"wind_scale", &GrassSettings::wind_scale},
	{"wind_dir_deg", &GrassSettings::wind_dir_deg},
	{"lean_spread_rad", &GrassSettings::lean_spread_rad},
	{"base_curve", &GrassSettings::base_curve},
	{"camera_tilt", &GrassSettings::camera_tilt},
	{"ring_width_gain", &GrassSettings::ring_width_gain},
	{"flower_chance", &GrassSettings::flower_chance},
	{"gloss", &GrassSettings::gloss},
};
struct IntField { const char *name; int GrassSettings::*member; };
const IntField kIntFields[] = {
	{"blades_per_brick", &GrassSettings::blades_per_brick},
	{"max_blades", &GrassSettings::max_blades},
};
} // namespace

void GrassSettingsStore::set(const GrassSettings &s) {
	GrassSettings copy = s;
	clamp_grass_settings(&copy);
	std::lock_guard<std::mutex> lock(mutex_);
	settings_ = copy;
}

GrassSettings GrassSettingsStore::get() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return settings_;
}

bool GrassSettingsStore::set_value(const char *name, float v) {
	if (!name) return false;
	std::lock_guard<std::mutex> lock(mutex_);
	if (std::strcmp(name, "enabled") == 0) {
		settings_.enabled = v != 0.0f;
		clamp_grass_settings(&settings_);
		return true;
	}
	for (const FloatField &f : kFloatFields) {
		if (std::strcmp(name, f.name) == 0) {
			settings_.*(f.member) = v;
			clamp_grass_settings(&settings_);
			return true;
		}
	}
	for (const IntField &f : kIntFields) {
		if (std::strcmp(name, f.name) == 0) {
			settings_.*(f.member) = static_cast<int>(v);
			clamp_grass_settings(&settings_);
			return true;
		}
	}
	return false;
}

float GrassSettingsStore::value(const char *name) const {
	if (!name) return 0.0f;
	std::lock_guard<std::mutex> lock(mutex_);
	if (std::strcmp(name, "enabled") == 0) return settings_.enabled ? 1.0f : 0.0f;
	for (const FloatField &f : kFloatFields) {
		if (std::strcmp(name, f.name) == 0) return settings_.*(f.member);
	}
	for (const IntField &f : kIntFields) {
		if (std::strcmp(name, f.name) == 0) return static_cast<float>(settings_.*(f.member));
	}
	return 0.0f;
}

} // namespace ve
