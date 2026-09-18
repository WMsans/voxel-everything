#pragma once

#include <cstddef>
#include <string>
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

struct FieldResources {};  // Plan A: no CPU-side resource sampling yet

// A stage's CPU mirror DECLARES the names it binds, through VE_STAGE_SLOTS and
// VE_STAGE_PARAMS below, and PipelineFieldGenerator::create resolves each one against the
// resolved pipeline. That is the whole point: the mirror used to index a positional
// extra[] array whose ordering rule ("writes in declaration order, then reads") lived in
// comments, so reordering a //!out line silently rebound every later index. A name the
// manifest does not declare now fails the load with the stage and the name in the message.
//
// The two blobs are built once at create() -- an int per declared slot, a float per
// declared param, in declaration order -- so the trampoline's cast reads exactly what the
// generator wrote, and per-sample cost is unchanged.
using StageFn = void (*)(FieldCtx &, const void *slots, const void *params,
		const FieldResources &);

struct StageBinding {
	std::string symbol;
	StageFn fn = nullptr;
	std::string slot_names;   // "p, sdf, height" -- exactly as spelled in VE_STAGE_SLOTS
	std::string param_names;  // "amp_a, amp_b, amp_c"; empty for a stage with no params
	size_t slot_size = 0;
	size_t param_size = 0;
};

// Splits a stringised __VA_ARGS__ list into trimmed names. Empty input yields no names.
std::vector<std::string> split_binding_names(const std::string &list);

class StageLibrary {
public:
	static StageLibrary &instance();
	void register_stage(const StageBinding &b);
	const StageBinding *lookup(const std::string &symbol) const;  // nullptr when absent
private:
	std::vector<StageBinding> entries_;
};

struct StageRegistrar {
	StageRegistrar(const char *symbol, StageFn fn, const char *slot_names,
			const char *param_names, size_t slot_size, size_t param_size);
};
} // namespace ve

// Declares the slot struct AND the name list the resolver uses. Name every channel the
// mirror touches, `p` included -- there are no built-ins to remember, because every name
// goes through ResolvedPipeline::channel_slot the same way.
#define VE_STAGE_SLOTS(Stage, ...)                        \
	struct Stage##Slots { int __VA_ARGS__; };             \
	inline const char *ve_slot_names(Stage##Slots *) { return #__VA_ARGS__; }

// __VA_OPT__ (C++20) is what lets a stage with no params declare an empty struct.
#define VE_STAGE_PARAMS(Stage, ...)                       \
	struct Stage##Params { __VA_OPT__(float __VA_ARGS__;) }; \
	inline const char *ve_param_names(Stage##Params *) { return #__VA_ARGS__; }

// `symbol` is the //!cpu name in the manifest; `Stage` is the prefix used above.
#define VE_REGISTER_STAGE(symbol, Stage, fn)                                          \
	static void ve_tramp_##fn(::ve::FieldCtx &ctx, const void *slots, const void *params, \
			const ::ve::FieldResources &res) {                                        \
		fn(ctx, *static_cast<const Stage##Slots *>(slots),                            \
				*static_cast<const Stage##Params *>(params), res);                    \
	}                                                                                 \
	static ::ve::StageRegistrar ve_stage_reg_##fn(symbol, &ve_tramp_##fn,             \
			ve_slot_names(static_cast<Stage##Slots *>(nullptr)),                      \
			ve_param_names(static_cast<Stage##Params *>(nullptr)), sizeof(Stage##Slots), \
			sizeof(Stage##Params))
