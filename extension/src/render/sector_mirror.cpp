#include "render/sector_mirror.h"
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <cstring>
#include <set>

using namespace godot;

namespace {
PackedByteArray bytes_of(const void *data, size_t n) {
	PackedByteArray b;
	b.resize(int64_t(n));
	if (n) std::memcpy(b.ptrw(), data, n);
	return b;
}
} // namespace

SectorMirror::~SectorMirror() { teardown(); }

bool SectorMirror::initialize(RenderingDevice *rd, int layers) {
	teardown();
	rd_ = rd;
	if (rd == nullptr || layers <= 0) return false;
	Ref<RDTextureFormat> fmt;
	fmt.instantiate();
	fmt->set_texture_type(RenderingDevice::TEXTURE_TYPE_2D_ARRAY);
	fmt->set_format(RenderingDevice::DATA_FORMAT_R16G16_UNORM);
	fmt->set_width(ve::kSectorStride);
	fmt->set_height(ve::kSectorStride);
	fmt->set_array_layers(layers);
	fmt->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT);
	Ref<RDTextureView> view;
	view.instantiate();
	array_ = rd->texture_create(fmt, view);
	const std::vector<int32_t> empty = ve::sector_window({0, 0}, {});
	window_ = rd->storage_buffer_create(int64_t(empty.size() * 4), bytes_of(empty.data(), empty.size() * 4));
	Ref<RDSamplerState> ss;
	ss.instantiate();
	ss->set_min_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	ss->set_mag_filter(RenderingDevice::SAMPLER_FILTER_NEAREST);
	sampler_ = rd->sampler_create(ss);
	if (!array_.is_valid() || !window_.is_valid() || !sampler_.is_valid()) {
		teardown();
		return false;
	}
	for (int i = layers - 1; i >= 0; i--) free_layers_.push_back(i);
	return true;
}

void SectorMirror::teardown() {
	if (rd_ != nullptr) {
		if (sampler_.is_valid()) rd_->free_rid(sampler_);
		if (window_.is_valid()) rd_->free_rid(window_);
		if (array_.is_valid()) rd_->free_rid(array_);
	}
	sampler_ = window_ = array_ = RID();
	layer_of_.clear();
	free_layers_.clear();
	synced_version_ = UINT64_MAX;
	synced_centre_ = {INT_MIN, INT_MIN};
	rd_ = nullptr;
}

void SectorMirror::sync(const ve::SectorCache &cache) {
	if (rd_ == nullptr || !array_.is_valid()) return;
	// Version first, then the snapshot: an insert landing between the two leaves the stored
	// version older than the bytes, so the next sync simply runs again.
	const uint64_t version = cache.version();
	const ve::SectorCoord centre = cache.centre();
	if (version == synced_version_ && centre == synced_centre_) return;
	const auto snap = cache.snapshot();
	std::set<ve::SectorCoord> live;
	for (const auto &e : snap) live.insert(e.first);
	for (auto it = layer_of_.begin(); it != layer_of_.end();) {
		if (!live.count(it->first)) {
			free_layers_.push_back(it->second);
			it = layer_of_.erase(it);
		} else {
			++it;
		}
	}
	for (const auto &e : snap) {
		if (layer_of_.count(e.first)) continue;
		// ponytail: capacity is the cache's max_resident(), so this only runs dry if the
		// radius grew after initialize(); the sector then stays absent and its regions held.
		if (free_layers_.empty()) break;
		const int layer = free_layers_.back();
		free_layers_.pop_back();
		rd_->texture_update(array_, layer,
				bytes_of(e.second->texels.data(), e.second->texels.size() * 4));
		layer_of_[e.first] = layer;
	}
	std::vector<ve::SectorLayer> layers;
	layers.reserve(layer_of_.size());
	for (const auto &kv : layer_of_) layers.push_back({kv.first, kv.second});
	const std::vector<int32_t> w = ve::sector_window(centre, layers);
	rd_->buffer_update(window_, 0, int64_t(w.size() * 4), bytes_of(w.data(), w.size() * 4));
	synced_version_ = version;
	synced_centre_ = centre;
}
