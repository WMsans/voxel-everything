#pragma once

#include <string>
#include <utility>
#include <vector>

namespace ve {
struct FieldCtx {
	static constexpr int kMaxChannels = 32;
	// Four floats per channel so a vec4 channel fits; scalars use component 0. Slot indices
	// come from ResolvedPipeline::channel_slot, so CPU and GLSL index identically.
	float ch[kMaxChannels * 4] = {};
	float &f(int slot) { return ch[slot * 4]; }
	float f(int slot) const { return ch[slot * 4]; }
	float *v(int slot) { return &ch[slot * 4]; }
	const float *v(int slot) const { return &ch[slot * 4]; }
};
struct StageParams {
	const float *values = nullptr;
	int count = 0;
	float at(int i) const { return (values != nullptr && i >= 0 && i < count) ? values[i] : 0.0f; }
};
struct FieldResources {};  // Plan A: no CPU-side resource sampling yet
struct StageSlots {
	int p = 0, sdf = 1, material = 2;
	int extra[FieldCtx::kMaxChannels] = {};  // per-stage resolved slot indices
};
using StageFn = void (*)(FieldCtx &, const StageSlots &, const StageParams &, const FieldResources &);

class StageLibrary {
public:
	static StageLibrary &instance();
	void register_stage(const std::string &symbol, StageFn fn);
	StageFn lookup(const std::string &symbol) const;  // nullptr when absent
private:
	std::vector<std::pair<std::string, StageFn>> entries_;
};

struct StageRegistrar { StageRegistrar(const char *symbol, StageFn fn); };
}
#define VE_REGISTER_STAGE(symbol, fn) \
	static ::ve::StageRegistrar ve_stage_reg_##fn(symbol, &fn)
