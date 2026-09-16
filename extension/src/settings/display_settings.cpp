#include "settings/display_settings.h"
#include <iterator>

namespace ve {
namespace {

// Same set demo/benchmark.gd's --upscaler= accepts, so a configuration found in the panel can be
// reproduced on the command line.
const char *const kUpscalerLabels[] = {"Bilinear", "FSR 1", "FSR 2", "MetalFX spatial", "MetalFX temporal"};
const char *const kResolutionLabels[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440", "3840 x 2160"};
static_assert(std::size(kUpscalerLabels) == kUpscalerOptionCount);
static_assert(std::size(kResolutionLabels) == std::size(kResolutions));

const SettingRow<DisplaySettings> kDisplayRows[] = {
		float_row("render_scale", "Render scale", &DisplaySettings::render_scale, 0.25f, 1.0f, 0.5f, 1.0f,
				0.01f, "Fraction of the window the 3D scene renders at before upscaling."),
		enum_row("upscaler", "Upscaler", &DisplaySettings::upscaler,
				std::span<const char *const>(kUpscalerLabels)),
		enum_row("resolution", "Resolution", &DisplaySettings::resolution,
				std::span<const char *const>(kResolutionLabels), -1),
		bool_row("fullscreen", "Fullscreen", &DisplaySettings::fullscreen),
};

} // namespace

int resolution_index_of(int w, int h) {
	for (int i = 0; i < static_cast<int>(std::size(kResolutions)); i++)
		if (kResolutions[i].w == w && kResolutions[i].h == h) return i;
	return -1;
}

std::span<const SettingRow<DisplaySettings>> display_rows() {
	return kDisplayRows;
}

} // namespace ve
