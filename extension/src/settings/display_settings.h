#pragma once
#include "settings/settings_store.h"
#include <span>

namespace ve {

// The window and upscaling dials (spec 2026-09-16 §3.3). VoxelSettings owns this store: its base
// is whatever the Viewport and window were when the node became ready, and it pushes every
// resolved value back to them.
struct DisplaySettings {
	float render_scale = 1.0f; // Viewport.scaling_3d_scale
	int upscaler = 0;          // index into the upscaler options; VoxelSettings maps it to a mode
	int resolution = -1;       // index into kResolutions; -1 = the window is not a preset size
	bool fullscreen = false;
};

struct WindowSize {
	int w;
	int h;
};

// Offered window sizes, ascending. project.godot ships 2560x1440.
inline constexpr WindowSize kResolutions[] = {
		{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3840, 2160},
};

inline constexpr int kUpscalerOptionCount = 5;

// -1 when (w, h) is not one of kResolutions: never rounded to a neighbour.
int resolution_index_of(int w, int h);

std::span<const SettingRow<DisplaySettings>> display_rows();

class DisplaySettingsStore : public SettingsStore<DisplaySettings> {
public:
	DisplaySettingsStore() :
			SettingsStore(display_rows(), nullptr, DisplaySettings{}) {}
};

} // namespace ve
