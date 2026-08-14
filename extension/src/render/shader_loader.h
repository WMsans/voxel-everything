#pragma once
#include <string>

namespace ve {

// Loads a GLSL file and expands `#include "name"` lines against include_dir,
// recursively. Detects include cycles. On failure returns "" and sets *error.
std::string load_shader_source(const std::string &path, const std::string &include_dir,
		std::string *error);

} // namespace ve
