#pragma once
#include <string>

#include "terrain/pipeline.h"

namespace ve {
// Emits the full replacement text for shaders/field.glslh. `prelude` is the text of the
// current field.glslh from "const uint OP_SPHERE_SUBTRACT" through the end of eval_field(),
// with base_field() removed -- the caller reads it from disk so this stays pure.
std::string generate_field_glslh(const ResolvedPipeline &p, const std::string &prelude);
}
