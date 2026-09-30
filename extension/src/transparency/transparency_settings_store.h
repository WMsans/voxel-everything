#pragma once
#include "transparency/transparency_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Transparency is its own module with its own store, mirroring LeafSettingsStore without
// joining it. No godot-cpp -- this file is in the native test target.
class TransparencySettingsStore : public SettingsStore<TransparencySettings> {
public:
	TransparencySettingsStore() :
			SettingsStore(transparency_rows(), nullptr, TransparencySettings{}) {}
};

} // namespace ve
