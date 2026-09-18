#pragma once
// The CPU half of the terrain pipeline. Walks the SAME ordered stage list the generated
// GLSL composes, over a FieldCtx whose channel slots the SAME compiler assigned -- so CPU
// and GPU agree by construction rather than by inspection.
#include "generator/generator.h"
#include "terrain/pipeline.h"
#include "terrain/stage_library.h"
#include <memory>
#include <string>
#include <vector>

namespace ve {

class PipelineFieldGenerator : public Generator {
public:
	static PipelineFieldGenerator *create(const ResolvedPipeline &p, std::string *error);

	Sample sample(float x, float y, float z) const override;

	// Central differences over the pipeline field, exactly as Generator::sample_gradient
	// computes them -- but reported EXACT. A stage pipeline has no analytic gradient;
	// finite differences are the gradient on both sides (the generated GLSL does the same
	// taps in base_field_gradient), and every exactness consumer treats the flag as
	// "usable as the surface normal", which this is to ~1e-4 away from a CSG crease -- and
	// at a crease both sides differentiate the identical taps, so they agree with each
	// other. Reporting inexact here would empty the consolidation normal payload and the
	// island fallback, and demote raymarch shading to the R8 fallback everywhere.
	FieldSample sample_gradient(float x, float y, float z) const override;

	float lipschitz() const override { return pipeline_.lipschitz; }
	bool is_cpu_exact() const { return pipeline_.cpu_exact; }
	const ResolvedPipeline &pipeline() const { return pipeline_; }

private:
	ResolvedPipeline pipeline_;
	std::vector<StageFn> fns_;
	// Each blob is allocated at max alignment and populated through memcpy. The registered
	// trampoline casts it back to its own implicit-lifetime aggregate type; StageFn itself
	// stays unchanged and the pointer is still passed directly at every sample.
	std::vector<std::shared_ptr<void>> slot_blobs_;
	std::vector<std::shared_ptr<void>> param_blobs_;
};

} // namespace ve
