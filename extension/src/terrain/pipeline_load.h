#pragma once
// The one read -> parse -> resolve sequence. It was written out three times (voxel_world,
// two native tests) and the three copies had already drifted in their error messages.
//
// Pure C++ (no godot-cpp), which is what keeps it in the native suite: the reader is the
// only Godot-aware part. VoxelWorld supplies a FileAccess reader over res://; the native
// tests supply an ifstream reader over the repo.
#include <functional>
#include <string>

#include "terrain/pipeline.h"

namespace ve {

// Returns false when `path` cannot be read; `*out` is only meaningful on true.
using TextReader = std::function<bool(const std::string &path, std::string *out)>;

// `pipeline_path` is passed to the reader verbatim. Stage paths are passed as
// `stage_root` + the path the pipeline file spells (e.g. "stages/hills.field.glslh"),
// because a pipeline lives under assets/ and its stages under shaders/.
bool load_pipeline(const TextReader &reader, const std::string &pipeline_path,
		const std::string &stage_root, ResolvedPipeline *out, std::string *error);

} // namespace ve
