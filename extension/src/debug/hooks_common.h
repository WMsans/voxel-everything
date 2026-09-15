#pragma once
// File-local helpers shared by the hooks*.cpp translation units.
#include "render/frame.h"
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <cstdint>
#include <cstring>

namespace godot {

// Half-precision to single-precision (normal + subnormal paths).
inline float half_to_float(uint16_t v) {
	const uint32_t sign = (v & 0x8000u) << 16;
	const uint32_t exp = (v >> 10) & 0x1F;
	const uint32_t mant = v & 0x3FF;
	if (exp == 0) return (sign ? -1.0f : 1.0f) * mant / 1024.0f / 16384.0f;
	uint32_t bits = sign | ((exp + 112) << 23) | (mant << 13);
	float f;
	std::memcpy(&f, &bits, 4);
	return f;
}

inline void write_frame_record(Dictionary &d, const FrameRecord &r) {
	d["fade_start"] = r.fade_start;
	d["fade_end"] = r.fade_end;
	d["two_phase"] = r.lod_two_phase;
	d["hiz_built"] = r.hiz_built;
	d["first_pass_count"] = r.lod_first_pass_count;
	PackedStringArray ok, cancelled;
	for (uint32_t s = 0; s < kStageCount; s++) {
		if (r.stages_ok & (1u << s)) ok.push_back(frame_stage_name(static_cast<FrameStage>(s)));
		if (r.stages_cancelled & (1u << s))
			cancelled.push_back(frame_stage_name(static_cast<FrameStage>(s)));
	}
	d["stages_ok"] = ok;
	d["stages_cancelled"] = cancelled;
}

} // namespace godot
