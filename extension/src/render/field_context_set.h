#pragma once
// Set 1: the terrain pipeline's context. Every field-consuming pass binds this beside its
// own untouched set 0. Binding 0 is the params UBO, binding 1 the sector-slot map (empty in
// Plan A; Plan B fills it), bindings 2..N the sampled context resources.
//
// Bound UNCONDITIONALLY, even when a pipeline declares no resources, so the binding code at
// ten call sites has no special case.
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "terrain/pipeline.h"

namespace godot {

class FieldContextSet {
public:
	~FieldContextSet();
	bool initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p);
	void teardown();
	bool is_valid() const { return uset_.is_valid(); }
	RID uniform_set() const { return uset_; }
	void bind(RenderingDevice *rd, int64_t list) const;

private:
	RenderingDevice *rd_ = nullptr;
	RID params_ubo_, sector_map_, uset_;
};

} // namespace godot
