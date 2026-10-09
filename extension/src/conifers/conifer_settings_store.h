#pragma once
#include "conifers/conifer_settings.h"
#include "settings/settings_store.h"

namespace ve {

// Conifers are their own module with their own store, as leaves are. No godot-cpp.
class ConiferSettingsStore : public SettingsStore<ConiferSettings> {
public:
	ConiferSettingsStore() :
			SettingsStore(conifer_rows(), nullptr, ConiferSettings{}) {}
};

} // namespace ve
