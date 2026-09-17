#pragma once
#include "settings/settings_store.h"
#include "shade/beauty_settings.h"

namespace ve {

// The beauty knobs: a quality tier's preset as the base, per-knob overrides on top. A tier change
// rebases (RenderOrchestrator), so overrides survive it. No godot-cpp -- in the native test target.
class BeautySettingsStore : public SettingsStore<BeautySettings> {
public:
	BeautySettingsStore() :
			SettingsStore(beauty_rows(), normalize_beauty, settings_for_tier(QualityTier::kHigh)) {}
};

} // namespace ve
