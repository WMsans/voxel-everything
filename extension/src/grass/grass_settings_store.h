#pragma once
#include "grass/grass_settings.h"
#include <mutex>

namespace ve {

// Mirrors the SHAPE of RenderOrchestrator's beauty_mutex_/beauty_snapshot() pair without
// joining BeautySettings: grass is its own module and the beauty stack must not grow a
// grass field (design doc section 7). No godot-cpp here -- this file is in the native test
// target.
class GrassSettingsStore {
public:
	void set(const GrassSettings &s);
	GrassSettings get() const;

	// Name-addressed access, so the demo menu and the debug hooks need no per-field
	// plumbing. Booleans are carried as 0.0 / 1.0. Returns false for an unknown name.
	bool set_value(const char *name, float v);
	float value(const char *name) const;

private:
	mutable std::mutex mutex_;
	GrassSettings settings_;
};

} // namespace ve
