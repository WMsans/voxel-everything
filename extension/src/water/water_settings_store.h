#pragma once
#include "water/water_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Water is its own module with its own store, mirroring TransparencySettingsStore without
// joining it. No godot-cpp -- this file is in the native test target.
class WaterSettingsStore : public SettingsStore<WaterSettings> {
public:
	WaterSettingsStore() : SettingsStore(water_rows(), nullptr, WaterSettings{}) {}
};

} // namespace ve
