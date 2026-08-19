#include "render/gpu_timings.h"

#include <godot_cpp/variant/string.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <sstream>
#include <vector>

using namespace godot;

namespace {

const char *const kPasses[] = {
		"raymarch", "composite", "lod", "sun_shadow", "ssgi", "deferred", "inject",
		"contact", "ssr", "outlines", "history"};

struct Marker {
	uint64_t serial = 0;
	std::string pass;
	int occurrence = 0;
	char side = 0;
};

bool parse_marker(const std::string &text, Marker *out) {
	if (text.rfind("ve:", 0) != 0) return false;
	std::vector<std::string> fields;
	std::stringstream stream(text);
	std::string field;
	while (std::getline(stream, field, ':')) fields.push_back(field);
	if (fields.size() != 5 || fields[0] != "ve" || fields[1].empty() || fields[2].empty() ||
			fields[3].empty() || (fields[4] != "b" && fields[4] != "e")) return false;
	char *end = nullptr;
	const unsigned long long serial = std::strtoull(fields[1].c_str(), &end, 10);
	if (!end || *end != '\0') return false;
	const long occurrence = std::strtol(fields[3].c_str(), &end, 10);
	if (!end || *end != '\0' || occurrence < 0) return false;
	out->serial = static_cast<uint64_t>(serial);
	out->pass = fields[2];
	out->occurrence = static_cast<int>(occurrence);
	out->side = fields[4][0];
	return true;
}

bool known_pass(const std::string &pass) {
	if (pass == "frame") return true;
	for (const char *name : kPasses) if (pass == name) return true;
	return false;
}

Dictionary empty_snapshot() {
	Dictionary result;
	for (const char *name : kPasses) result[String(name) + "_gpu_ms"] = -1.0;
	result["custom_frame_gpu_ms"] = -1.0;
	result["valid"] = false;
	result["sample_id"] = 0;
	result["render_device_frame"] = -1;
	result["captured_serial"] = -1;
	result["dropped_pairs"] = 0;
	// ingest_for_test() receives explicit microseconds and never applies live normalization.
	result["timestamp_unit"] = "synthetic_microseconds";
	result["timestamp_scale_to_microseconds"] = 1.0;
	result["timestamp_normalization"] = "none_synthetic_inputs_are_microseconds";
	result["timestamp_normalized"] = false;
	return result;
}

std::string marker_name(uint64_t serial, const char *pass, int occurrence, char side) {
	return "ve:" + std::to_string(serial) + ":" + pass + ":" +
			std::to_string(occurrence) + ":" + side;
}

} // namespace

void GpuTimings::begin_frame(RenderingDevice *rd) {
	poll(rd);
	if (!rd) return;
	std::lock_guard<std::mutex> lock(mutex_);
	serial_++;
	next_.clear();
	active_.clear();
	const std::string marker = marker_name(serial_, "frame", 0, 'b');
	active_["frame"] = 0;
	next_["frame"] = 1;
	rd->capture_timestamp(String(marker.c_str()));
}

void GpuTimings::end_frame(RenderingDevice *rd) {
	if (!rd) return;
	std::lock_guard<std::mutex> lock(mutex_);
	const auto it = active_.find("frame");
	if (it == active_.end()) return;
	rd->capture_timestamp(String(marker_name(serial_, "frame", it->second, 'e').c_str()));
	// A pass that forgot to close is a dropped pair, never an active marker carried into the
	// next frame. The unmatched begin remains in the capture query for diagnostics.
	active_.clear();
}

void GpuTimings::begin(RenderingDevice *rd, const char *pass) {
	if (!rd || !pass || !*pass) return;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string name(pass);
	const int occurrence = next_[name]++;
	active_[name] = occurrence;
	rd->capture_timestamp(String(marker_name(serial_, pass, occurrence, 'b').c_str()));
}

void GpuTimings::end(RenderingDevice *rd, const char *pass) {
	if (!rd || !pass || !*pass) return;
	std::lock_guard<std::mutex> lock(mutex_);
	const std::string name(pass);
	const auto it = active_.find(name);
	if (it == active_.end()) return;
	rd->capture_timestamp(String(marker_name(serial_, pass, it->second, 'e').c_str()));
	active_.erase(it);
}

void GpuTimings::cancel(const char *pass) {
	if (!pass || !*pass) return;
	std::lock_guard<std::mutex> lock(mutex_);
	active_.erase(pass);
}

void GpuTimings::abort_frame() {
	std::lock_guard<std::mutex> lock(mutex_);
	active_.clear();
}

namespace {

// Target backend normalization: Godot 4.7.1 on Vulkan/RTX 4070 exposes raw GPU timestamp
// values in nanoseconds in this environment. Convert them deterministically to the documented
// microsecond unit exactly once before the common parser divides by 1000 for milliseconds.
// This is intentionally not inferred from CPU or wall-clock timing.
constexpr double kLiveVulkanNanosecondsToMicroseconds = 0.001;

int64_t normalize_live_gpu_us(uint64_t raw) {
	const double normalized = static_cast<double>(raw) * kLiveVulkanNanosecondsToMicroseconds;
	if (!(normalized > 0.0)) return 0;
	if (normalized >= static_cast<double>(std::numeric_limits<int64_t>::max()))
		return std::numeric_limits<int64_t>::max();
	return static_cast<int64_t>(std::llround(normalized));
}

} // namespace

void GpuTimings::poll(RenderingDevice *rd) {
	if (!rd) return;
	const uint64_t frame = rd->get_captured_timestamps_frame();
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (frame == last_rd_frame_) return;
	}
	const uint32_t count = rd->get_captured_timestamps_count();
	PackedStringArray names;
	PackedInt64Array normalized;
	names.resize(count);
	normalized.resize(count);
	for (uint32_t i = 0; i < count; i++) {
		names.set(i, rd->get_captured_timestamp_name(i));
		normalized.set(i, normalize_live_gpu_us(rd->get_captured_timestamp_gpu_time(i)));
	}
	// Updating this even for an empty capture prevents repeatedly consuming a frame whose
	// timestamp query has already been attempted. A later RD frame will supersede it.
	if (count > 0) {
		Dictionary result = ingest_for_test(names, normalized, frame);
		std::lock_guard<std::mutex> lock(mutex_);
		result["timestamp_unit"] = "live_vulkan_nanoseconds_normalized";
		result["timestamp_scale_to_microseconds"] = 0.001;
		result["timestamp_normalization"] =
				"deterministic_vulkan_nanoseconds_to_microseconds";
		result["timestamp_normalized"] = true;
		latest_ = result;
	} else {
		std::lock_guard<std::mutex> lock(mutex_);
		last_rd_frame_ = frame;
	}
}

Dictionary GpuTimings::ingest_for_test(const PackedStringArray &names,
		const PackedInt64Array &gpu_us, uint64_t rd_frame) {
	std::lock_guard<std::mutex> lock(mutex_);
	Dictionary result = empty_snapshot();
	const int count = std::min(names.size(), gpu_us.size());
	std::map<uint64_t, std::map<std::string, std::map<int, std::pair<int64_t, int64_t>>>> samples;
	for (int i = 0; i < count; i++) {
		Marker marker;
		if (!parse_marker(names[i].utf8().get_data(), &marker) || !known_pass(marker.pass)) continue;
		auto &pair = samples[marker.serial][marker.pass][marker.occurrence];
		if (marker.side == 'b') pair.first = gpu_us[i];
		else pair.second = gpu_us[i];
	}

	uint64_t selected_serial = 0;
	bool frame_complete = false;
	std::map<std::string, std::map<int, std::pair<int64_t, int64_t>>> *selected = nullptr;
	for (auto it = samples.rbegin(); it != samples.rend(); ++it) {
		const auto frame_it = it->second.find("frame");
		if (frame_it == it->second.end()) continue;
		const auto pair_it = frame_it->second.find(0);
		if (pair_it == frame_it->second.end()) continue;
		const int64_t begin_us = pair_it->second.first;
		const int64_t end_us = pair_it->second.second;
		if (begin_us > 0 && end_us > begin_us) {
			selected_serial = it->first;
			selected = &it->second;
			frame_complete = true;
			break;
		}
	}
	if (!selected) {
		// Still report pairs from the newest serial so missing/non-positive values are
		// visible as dropped rather than silently looking like zero-cost passes.
		if (!samples.empty()) {
			selected_serial = samples.rbegin()->first;
			selected = &samples.rbegin()->second;
		}
	}

	int dropped = 0;
	if (selected) {
		for (const auto &pass_entry : *selected) {
			const std::string &pass = pass_entry.first;
			for (const auto &occurrence_entry : pass_entry.second) {
				const int64_t begin_us = occurrence_entry.second.first;
				const int64_t end_us = occurrence_entry.second.second;
				if (begin_us <= 0 || end_us <= begin_us) {
					dropped++;
					continue;
				}
				if (pass == "frame") continue;
				const double milliseconds = static_cast<double>(end_us - begin_us) / 1000.0;
				const String key = String(pass.c_str()) + "_gpu_ms";
				result[key] = static_cast<double>(result.get(key, -1.0)) < 0.0
						? milliseconds : static_cast<double>(result[key]) + milliseconds;
			}
		}
		if (frame_complete) {
			const auto frame_pair = (*selected)["frame"][0];
			result["custom_frame_gpu_ms"] = static_cast<double>(frame_pair.second - frame_pair.first) / 1000.0;
		}
	}

	const bool new_frame = rd_frame != last_rd_frame_;
	last_rd_frame_ = rd_frame;
	if (frame_complete && new_frame) sample_id_++;
	result["valid"] = frame_complete;
	result["sample_id"] = static_cast<int64_t>(sample_id_);
	result["render_device_frame"] = static_cast<int64_t>(rd_frame);
	result["captured_serial"] = selected ? static_cast<int64_t>(selected_serial) : -1;
	result["dropped_pairs"] = dropped;
	latest_ = result;
	return result;
}

Dictionary GpuTimings::snapshot() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return latest_.is_empty() ? empty_snapshot() : latest_;
}
