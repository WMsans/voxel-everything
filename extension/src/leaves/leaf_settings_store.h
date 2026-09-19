#pragma once
#include "leaves/leaf_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Leaves are their own module with their own store, mirroring GrassSettingsStore without
// joining it. No godot-cpp -- this file is in the native test target.
class LeafSettingsStore : public SettingsStore<LeafSettings> {
public:
	LeafSettingsStore() :
			SettingsStore(leaf_rows(), nullptr, LeafSettings{}) {}
};

} // namespace ve
