#include "terrain/pipeline_field_generator.h"

namespace ve {

PipelineFieldGenerator *PipelineFieldGenerator::create(const ResolvedPipeline &p,
		std::string *error) {
	if (int(p.channels.size()) > FieldCtx::kMaxChannels) {
		if (error) *error = "pipeline declares more channels than FieldCtx::kMaxChannels";
		return nullptr;
	}
	PipelineFieldGenerator *g = new PipelineFieldGenerator();
	g->pipeline_ = p;

	int cursor = 0;
	for (const StageManifest &s : p.stages) {
		StageFn fn = nullptr;
		if (!s.cpu_symbol.empty()) {
			fn = StageLibrary::instance().lookup(s.cpu_symbol);
			if (fn == nullptr) {
				if (error) *error = "stage '" + s.name + "' names an unregistered cpu symbol: " +
						s.cpu_symbol;
				delete g;
				return nullptr;
			}
		}
		g->fns_.push_back(fn);

		StageSlots slots;
		slots.p = p.channel_slot("p");
		slots.sdf = p.channel_slot("sdf");
		slots.material = p.channel_slot("material");
		// extra[i] is the slot of this stage's i-th declared write, then its reads, in
		// declaration order -- the same order the generated GLSL names them.
		int n = 0;
		for (const ChannelDecl &w : s.writes)
			if (n < FieldCtx::kMaxChannels) slots.extra[n++] = p.channel_slot(w.name);
		for (const ChannelDecl &r : s.reads)
			if (n < FieldCtx::kMaxChannels) slots.extra[n++] = p.channel_slot(r.name);
		g->slots_.push_back(slots);

		g->param_base_.push_back(cursor);
		g->param_count_.push_back(int(s.params.size()));
		cursor += int(s.params.size());
	}
	for (const ParamDecl &pm : p.params) g->param_values_.push_back(pm.value);
	return g;
}

Sample PipelineFieldGenerator::View::sample(float x, float y, float z) const {
	FieldCtx ctx;
	const ResolvedPipeline &p = owner_->pipeline_;
	const int pslot = p.channel_slot("p");
	ctx.v(pslot)[0] = x;
	ctx.v(pslot)[1] = y;
	ctx.v(pslot)[2] = z;

	for (size_t i = 0; i < owner_->fns_.size(); i++) {
		StageFn fn = owner_->fns_[i];
		if (fn == nullptr) continue;  // GPU-only stage: the CPU field is already inexact
		StageParams sp;
		sp.values = owner_->param_values_.empty() ? nullptr
				: &owner_->param_values_[size_t(owner_->param_base_[i])];
		sp.count = owner_->param_count_[i];
		FieldResources res;
		fn(ctx, owner_->slots_[i], sp, res);
	}

	Sample s{};
	s.sdf = ctx.f(p.channel_slot("sdf"));
	s.material = uint16_t(ctx.f(p.channel_slot("material")));
	return s;
}

} // namespace ve
