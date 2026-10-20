#include "terrain/pipeline_field_generator.h"

#include <cstddef>
#include <cstring>
#include <new>

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

std::shared_ptr<void> make_blob(const void *words, size_t word_bytes, size_t size) {
	void *storage = ::operator new(size, std::align_val_t(alignof(std::max_align_t)));
	std::memset(storage, 0, size);
	if (word_bytes != 0) std::memcpy(storage, words, word_bytes);
	return std::shared_ptr<void>(storage, [](void *p) {
		::operator delete(p, std::align_val_t(alignof(std::max_align_t)));
	});
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
		if (s.kind == StageKind::kMap) {
			// Baked on the GPU and read back into the cache; the CPU never runs it.
			g->fns_.push_back(nullptr);
			g->slot_blobs_.push_back(nullptr);
			g->param_blobs_.push_back(nullptr);
			continue;
		}
		if (s.cpu_symbol.empty()) {
			// GPU-only stage: the CPU field is already inexact, and sample() skips it.
			g->fns_.push_back(nullptr);
			g->slot_blobs_.push_back(nullptr);
			g->param_blobs_.push_back(nullptr);
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
		g->slot_blobs_.push_back(make_blob(slots.data(), slots.size() * sizeof(int), b->slot_size));

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
		g->param_blobs_.push_back(make_blob(params.data(), params.size() * sizeof(float), b->param_size));
	}
	return g;
}

void PipelineFieldGenerator::eval_ctx(FieldCtx &ctx, float x, float y, float z) const {
	ctx = FieldCtx{};
	const ResolvedPipeline &p = pipeline_;
	const int pslot = p.channel_slot("p");
	// The world seed's domain shift, the same whole metres the GPU adds (VE_FIELD_OFFSET).
	ctx.v(pslot)[0] = x + float(p.field_offset_x);
	ctx.v(pslot)[1] = y;
	ctx.v(pslot)[2] = z + float(p.field_offset_z);

	for (size_t i = 0; i < fns_.size(); i++) {
		StageFn fn = fns_[i];
		if (fn == nullptr) continue;  // GPU-only stage: the CPU field is already inexact
		FieldResources res{sectors_.get()};
		fn(ctx, slot_blobs_[i].get(), param_blobs_[i].get(), res);
	}
}

Sample PipelineFieldGenerator::sample(float x, float y, float z) const {
	const ResolvedPipeline &p = pipeline_;
	FieldCtx ctx;
	eval_ctx(ctx, x, y, z);
	Sample s{};
	s.sdf = ctx.f(p.channel_slot("sdf"));
	s.material = uint16_t(ctx.f(p.channel_slot("material")));
	return s;
}

bool PipelineFieldGenerator::has_ground() const {
	return pipeline_.channel_slot("ground_sdf") >= 0;
}

Sample PipelineFieldGenerator::sample_ground(float x, float y, float z) const {
	const ResolvedPipeline &p = pipeline_;
	const int gslot = p.channel_slot("ground_sdf");
	if (gslot < 0) return sample(x, y, z);
	FieldCtx ctx;
	eval_ctx(ctx, x, y, z);
	Sample s{};
	s.sdf = ctx.f(gslot);
	s.material = uint16_t(ctx.f(p.channel_slot("ground_material")));
	return s;
}

bool PipelineFieldGenerator::liquid_level(float x, float z, float *y) const {
	const ResolvedPipeline &p = pipeline_;
	const int lslot = p.channel_slot("liquid_level");
	if (lslot < 0) return false;
	FieldCtx ctx;
	eval_ctx(ctx, x, ve::kSurfaceY, z);
	if (y) *y = ctx.f(lslot);
	return true;
}

FieldSample PipelineFieldGenerator::sample_gradient(float x, float y, float z) const {
	// Differentiate the ground view when the pipeline declares one, so underwater ground
	// normals stay the ground's (never the water plane's). For pipelines without
	// ground_sdf, sample_ground() delegates to sample(), so this is unchanged.
	const Sample s = sample_ground(x, y, z);
	const float e = 0.01f;
	const float dx = (sample_ground(x + e, y, z).sdf - sample_ground(x - e, y, z).sdf) / (2.0f * e);
	const float dy = (sample_ground(x, y + e, z).sdf - sample_ground(x, y - e, z).sdf) / (2.0f * e);
	const float dz = (sample_ground(x, y, z + e).sdf - sample_ground(x, y, z - e).sdf) / (2.0f * e);
	FieldSample fs{};
	fs.sdf = s.sdf;
	fs.material = s.material;
	fs.gradient[0] = dx;
	fs.gradient[1] = dy;
	fs.gradient[2] = dz;
	fs.exact_gradient = true;
	return fs;
}

} // namespace ve
