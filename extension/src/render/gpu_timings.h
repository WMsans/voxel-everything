#pragma once

#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace godot {

class GpuTimings {
public:
	void poll(RenderingDevice *);
	void begin_frame(RenderingDevice *);
	void end_frame(RenderingDevice *);
	void begin(RenderingDevice *, const char *);
	void end(RenderingDevice *, const char *);
	// Remove an active marker after a pass fails. The unmatched begin remains visible to
	// the parser as a dropped pair, but no stale marker can leak into the next frame.
	void cancel(const char *);
	void abort_frame();
	Dictionary snapshot() const;
	Dictionary ingest_for_test(const PackedStringArray &, const PackedInt64Array &, uint64_t);

private:
	uint64_t serial_ = 0;
	uint64_t last_rd_frame_ = UINT64_MAX;
	uint64_t sample_id_ = 0;
	std::map<std::string, int> next_, active_;
	mutable std::mutex mutex_;
	Dictionary latest_;
};

} // namespace godot
