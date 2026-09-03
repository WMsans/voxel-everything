#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ve {
struct PipelineStageRef {
    std::string path;                              // e.g. "stages/hills.field.glslh"
    std::vector<std::pair<std::string, float>> param_overrides;
};
struct PipelineDesc {
    uint32_t seed = 1337;
    float lipschitz_override = 0.0f;               // 0 => use the combined bound
    bool allow_gpu_only = false;
    std::vector<PipelineStageRef> stages;
};
bool parse_pipeline_desc(const std::string &source, PipelineDesc *out, std::string *error);
}
