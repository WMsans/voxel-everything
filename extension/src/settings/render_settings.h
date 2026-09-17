#pragma once
#include "settings/settings_store.h"
#include <span>

namespace ve {

// The render budget dials (spec 2026-09-16 §3.3). Not beauty knobs: they decide what the frame
// renders and at what cost. RenderOrchestrator mirrors near_field_scale, near_field and islands
// into atomics so the render thread reads them without this store's lock.
struct RenderSettings {
	int quality_tier = 3;           // ve::QualityTier; a change rebases the beauty store
	float near_field_scale = 0.66f; // fraction of the internal resolution the near-field marcher runs at
	bool near_field = true;
	bool islands = true;
};

std::span<const SettingRow<RenderSettings>> render_rows();

class RenderSettingsStore : public SettingsStore<RenderSettings> {
public:
	RenderSettingsStore() :
			SettingsStore(render_rows(), nullptr, RenderSettings{}) {}
};

} // namespace ve
