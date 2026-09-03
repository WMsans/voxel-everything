#pragma once
// The CPU half of the terrain pipeline. Walks the SAME ordered stage list the generated
// GLSL composes, over a FieldCtx whose channel slots the SAME compiler assigned -- so CPU
// and GPU agree by construction rather than by inspection.
#include "generator/field_generator.h"
#include "terrain/pipeline.h"
#include "terrain/stage_library.h"
#include <string>
#include <vector>

namespace ve {

class PipelineFieldGenerator : public FieldGenerator {
public:
	static PipelineFieldGenerator *create(const ResolvedPipeline &p, std::string *error);

	Sample eval(float x, float y, float z) const override { return view_.sample(x, y, z); }
	const Generator &sampler() const override { return view_; }
	bool is_cpu_exact() const { return pipeline_.cpu_exact; }
	float lipschitz() const { return pipeline_.lipschitz; }
	const ResolvedPipeline &pipeline() const { return pipeline_; }

private:
	// brick_eval, raycast and extract_island_volume all consume a `const ve::Generator &`;
	// this inner view is what keeps those call sites' signatures unchanged.
	class View : public Generator {
	public:
		explicit View(const PipelineFieldGenerator *owner) : owner_(owner) {}
		Sample sample(float x, float y, float z) const override;
		float lipschitz() const override { return owner_->pipeline_.lipschitz; }
	private:
		const PipelineFieldGenerator *owner_;
	};

	ResolvedPipeline pipeline_;
	std::vector<StageFn> fns_;              // parallel to pipeline_.stages
	std::vector<StageSlots> slots_;         // parallel to pipeline_.stages
	std::vector<float> param_values_;       // flattened, in pipeline_.params order
	std::vector<int> param_base_;           // parallel to stages: first param index
	std::vector<int> param_count_;
	View view_{this};

	friend class View;
};

} // namespace ve
