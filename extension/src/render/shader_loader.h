#pragma once
#include <string>

namespace ve {

// Loads a GLSL file and expands `#include "name"` lines against include_dir,
// recursively. Detects include cycles. On failure returns "" and sets *error.
std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error);

// Godot's shader_compile_spirv_from_source feeds GLSL to glslang, which rejects the
// Godot-only `#[compute]` annotation. Strips any line whose first non-space char is '#'
// followed by '['. Shared by every compute pass that keeps the annotation verbatim.
std::string strip_shader_annotations(const std::string &src);

// Test/dev override map consulted by load_shader_source. Keys are matched against the
// full path and its basename, so a test can inject "raymarch.comp.glsl" without knowing
// the absolute path. Used by the shader-reload pre-flight to prove a new source compiles
// before tearing down the last-known-good pipelines.
void set_shader_source_override(const std::string &key, const std::string &source);
void clear_shader_source_override(const std::string &key);
void clear_shader_source_overrides();

} // namespace ve
