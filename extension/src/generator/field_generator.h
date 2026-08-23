#pragma once
// FieldGenerator -- the world-generation seam (spec §4). Procedural G() is the first
// implementation; future worldgen features implement this interface and are swapped in
// pre-init via VoxelWorld::set_generator(). WorldStore holds the instance and owns its
// lifetime.
//
// Pure port (Task 10): nothing about generation changes. The concrete class delegates to
// today's ve::AnalyticGenerator, and the CPU evaluators keep walking a `const Generator &`
// through sampler(), so every call site behaves exactly as before.
#include "generator/generator.h"

namespace ve {

class FieldGenerator {
public:
	virtual ~FieldGenerator() = default;
	// The current generator entry point, same signature as ve::Generator::sample.
	virtual Sample eval(float x, float y, float z) const = 0;
	// The sampler the CPU evaluators take: brick_eval, raycast and extract_island_volume
	// all consume a `const ve::Generator &`, so implementations hand that view out
	// unchanged rather than forcing those call sites to move.
	virtual const Generator &sampler() const = 0;
};

// Today's analytic terrain, wrapped verbatim: eval() delegates to AnalyticGenerator.
class ProceduralFieldGenerator : public FieldGenerator {
public:
	Sample eval(float x, float y, float z) const override { return gen_.sample(x, y, z); }
	const Generator &sampler() const override { return gen_; }

private:
	AnalyticGenerator gen_;
};

} // namespace ve
