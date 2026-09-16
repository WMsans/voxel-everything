#pragma once
#include "grass/grass_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Grass is its own module with its own store (design doc section 7): it shares the settings
// mechanism with the beauty stack, never the struct. No godot-cpp -- this file is in the native
// test target.
class GrassSettingsStore : public SettingsStore<GrassSettings> {
public:
	GrassSettingsStore() :
			SettingsStore(grass_rows(), nullptr, GrassSettings{}) {}
};

} // namespace ve
