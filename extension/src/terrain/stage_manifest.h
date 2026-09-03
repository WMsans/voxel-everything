// A stage's manifest lives in its own GLSL source so the two cannot drift:
// the //! directives at the top of the .glsl file declare the stage's
// inputs, outputs, params and tuning, and the body below is compiled as-is.
#pragma once

#include <string>
#include <vector>

namespace ve {
enum class StageKind { kField, kMap };
enum class ChannelType { kFloat, kVec2, kVec3, kVec4, kInt, kUint };
int channel_component_count(ChannelType t);   // 1,2,3,4,1,1
const char *channel_glsl_type(ChannelType t); // "float","vec2","vec3","vec4","int","uint"

struct ChannelDecl { std::string name; ChannelType type = ChannelType::kFloat; };
struct ResourceDecl { std::string name, type; float fallback = 0.0f; };
struct ParamDecl { std::string name; ChannelType type = ChannelType::kFloat; float value = 0.0f; };

struct StageManifest {
    std::string name;
    StageKind kind = StageKind::kField;
    std::vector<ChannelDecl> reads, writes;
    std::vector<ResourceDecl> samples;
    std::vector<ParamDecl> params;
    float lipschitz = 1.0f;
    float bounds = 0.0f;
    int iterate = 1;
    std::string domain;
    int domain_w = 0, domain_h = 0;
    std::string cpu_symbol;  // empty => GPU-only
    std::string body;        // everything after the directive block, verbatim
};

bool parse_stage_manifest(const std::string &source, StageManifest *out, std::string *error);
}
