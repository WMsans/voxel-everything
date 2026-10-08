#pragma once
// One device's copy of the resident sectors (spec §4.3–4.4, plan deviation 1): a
// texture2d_array filled from the host SectorCache, and the toroidal window the field stages
// read through set 1 binding 1. The render device and the mesher's worker device each own
// one. The bytes come from the cache, so the two devices cannot disagree.
#include "terrain/sector_cache.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <climits>
#include <cstdint>
#include <map>
#include <vector>

namespace godot {

class SectorMirror {
public:
	~SectorMirror();
	// `layers` is the cache's max_resident(): the array never grows, so the uniform sets that
	// bind it never need rebuilding.
	bool initialize(RenderingDevice *rd, int layers);
	void teardown();
	// Uploads every cached sector not yet on this device, frees the layers of sectors the
	// cache dropped, and rewrites the window when either changed or the centre moved. Call
	// outside any open compute list.
	void sync(const ve::SectorCache &cache);
	bool has(ve::SectorCoord c) const { return layer_of_.count(c) != 0; }
	RID array() const { return array_; }
	RID window() const { return window_; }
	RID sampler() const { return sampler_; }
	int uploaded() const { return int(layer_of_.size()); }

private:
	RenderingDevice *rd_ = nullptr;
	RID array_, window_, sampler_;
	std::map<ve::SectorCoord, int> layer_of_;
	std::vector<int> free_layers_;
	uint64_t synced_version_ = UINT64_MAX;
	ve::SectorCoord synced_centre_{INT_MIN, INT_MIN};
};

} // namespace godot
