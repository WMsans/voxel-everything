#pragma once
#include <cstdint>
#include <vector>

#include "terrain/pipeline.h"

namespace ve {
// Shared dense-in-112 params packing for the field context uniform set.
//
// Values are DENSE (one float per 4 bytes, in pipeline order) in a buffer sized to
// Godot's 16-byte-stride expectation (16 bytes per param). The GLSL is std140, whose
// consecutive scalars pack densely -- and the driver reads them densely -- but Godot's
// uniform-set validation wants the 16-stride size, so a short buffer is rejected and a
// strided-values buffer misreads. A pipeline with no params still gets one vec4, so the
// buffer is never empty -- RenderingDevice rejects a zero-byte uniform buffer.
//
// Pure C++ (no godot-cpp): FieldContextSet::initialize and
// VoxelDebugHooks::debug_field_params_bytes both convert the result to PackedByteArray.
std::vector<uint8_t> pack_field_params_bytes(const ResolvedPipeline &p);
} // namespace ve
