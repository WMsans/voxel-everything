#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "terrain/stage_manifest.h"

namespace ve {
struct PipelineStageRef {
    std::string path;                              // e.g. "stages/hills.field.glslh"
    std::vector<std::pair<std::string, float>> param_overrides;
};
struct PipelineDesc {
    // The WORLD seed, set by load_pipeline's caller -- never parsed from the file.
    uint32_t seed = 0;
    // A CEILING, not an override: resolve computes the bound from the stages and refuses a
    // pipeline whose computed bound exceeds this. 0 => no ceiling declared, no check.
    float lipschitz_ceiling = 0.0f;
    bool allow_gpu_only = false;
    std::vector<PipelineStageRef> stages;
};
bool parse_pipeline_desc(const std::string &source, PipelineDesc *out, std::string *error);
struct ResolvedChannel { std::string name; ChannelType type; int slot; };  // slot: index into FieldCtx::ch
struct ResolvedPipeline {
    std::vector<StageManifest> stages;      // pipeline order
    std::vector<ResolvedChannel> channels;  // p, sdf, material first, then declared order
    std::vector<ResourceDecl> resources;    // sorted by name; set-1 binding = 2 + index
    std::vector<ParamDecl> params;          // flattened "<stage>.<param>", resolved values
    float lipschitz = 2.0f;
    bool cpu_exact = true;
    // Non-fatal problems the caller should surface. Resolve stays pure C++ (no godot-cpp),
    // so it collects text rather than calling push_warning itself.
    std::vector<std::string> warnings;
    uint64_t hash = 0;                      // FNV-1a over every stage body + resolved params
    uint32_t seed = 0;
    // seed_offset(seed), added to every sample position before the first stage runs, on
    // both sides: generate_field_glslh's VE_FIELD_OFFSET and PipelineFieldGenerator::sample.
    int32_t field_offset_x = 0;
    int32_t field_offset_z = 0;
    int channel_slot(const std::string &name) const;  // -1 when absent
};
bool resolve_pipeline(const PipelineDesc &desc, const std::vector<StageManifest> &loaded,
                      ResolvedPipeline *out, std::string *error);
}
