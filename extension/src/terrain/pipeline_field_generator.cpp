#include "terrain/pipeline_field_generator.h"

namespace ve {

namespace {
// A mirror's param name is either this stage's own ("<stage>.<name>") or a cross-stage use
// spelled the way the GLSL spells it -- "hills_amp_a" for "hills.amp_a", which is exactly
// how the params UBO flattens them. Task: //!use declares which of these are legal; this
// only has to find the value.
int find_param(const ResolvedPipeline &p, const std::string &stage, const std::string &name) {
	const std::string own = stage + "." + name;
	for (size_t i = 0; i < p.params.size(); i++)
		if (p.params[i].name == own) return int(i);
	for (size_t i = 0; i < p.params.size(); i++) {
		std::string ident = p.params[i].name;
		for (char &c : ident) if (c == '.') c = '_';
		if (ident == name) return int(i);
	}
	return -1;
}
} // namespace

PipelineFieldGenerator *PipelineFieldGenerator::create(const ResolvedPipeline &p,
		std::string *error) {
	if (int(p.channels.size()) > FieldCtx::kMaxChannels) {
		if (error) *error = "pipeline declares more channels than FieldCtx::kMaxChannels";
		return nullptr;
	}
	PipelineFieldGenerator *g = new PipelineFieldGenerator();
	g->pipeline_ = p;

	for (const StageManifest &s : p.stages) {
		if (s.cpu_symbol.empty()) {
			// GPU-only stage: the CPU field is already inexact, and sample() skips it.
			g->fns_.push_back(nullptr);
			g->slot_words_.emplace_back();
			g->param_words_.emplace_back(1, 0.0f);
			continue;
		}
		const StageBinding *b = StageLibrary::instance().lookup(s.cpu_symbol);
		if (b == nullptr) {
			if (error) *error = "stage '" + s.name + "' names an unregistered cpu symbol: " +
					s.cpu_symbol;
			delete g;
			return nullptr;
		}
		g->fns_.push_back(b->fn);

		std::vector<int> slots;
		for (const std::string &n : split_binding_names(b->slot_names)) {
			const int slot = p.channel_slot(n);
			if (slot < 0) {
				if (error) *error = "stage '" + s.name + "' cpu mirror binds channel '" + n +
						"', which this pipeline does not declare";
				delete g;
				return nullptr;
			}
			slots.push_back(slot);
		}
		g->slot_words_.push_back(slots);

		std::vector<float> params;
		for (const std::string &n : split_binding_names(b->param_names)) {
			const int idx = find_param(p, s.name, n);
			if (idx < 0) {
				if (error) *error = "stage '" + s.name + "' cpu mirror binds param '" + n +
						"', which neither this stage nor a //!use declares";
				delete g;
				return nullptr;
			}
			params.push_back(p.params[size_t(idx)].value);
		}
		if (params.empty()) params.push_back(0.0f);
		g->param_words_.push_back(params);
	}
	return g;
}

Sample PipelineFieldGenerator::sample(float x, float y, float z) const {
	FieldCtx ctx;
	const ResolvedPipeline &p = pipeline_;
	const int pslot = p.channel_slot("p");
	ctx.v(pslot)[0] = x;
	ctx.v(pslot)[1] = y;
	ctx.v(pslot)[2] = z;

	for (size_t i = 0; i < fns_.size(); i++) {
		StageFn fn = fns_[i];
		if (fn == nullptr) continue;  // GPU-only stage: the CPU field is already inexact
		FieldResources res;
		fn(ctx, slot_words_[i].data(), param_words_[i].data(), res);
	}

	Sample s{};
	s.sdf = ctx.f(p.channel_slot("sdf"));
	s.material = uint16_t(ctx.f(p.channel_slot("material")));
	return s;
}

FieldSample PipelineFieldGenerator::sample_gradient(float x, float y, float z) const {
	// The base implementation already differentiates through THIS view's sample() (the
	// pipeline field) with the same epsilon the GPU uses; only the flag changes (see the
	// header for why exact is the honest report here).
	FieldSample fs = Generator::sample_gradient(x, y, z);
	fs.exact_gradient = true;
	return fs;
}

} // namespace ve
