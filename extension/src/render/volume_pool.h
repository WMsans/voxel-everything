#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include "generator/volume_set.h"

namespace godot {

// One device's mirror of ve::VolumeSet. Two storage buffers -- SDF bytes and material bytes
// -- each holding `slots` lattices of `dim`^3 back to back, so a slot's offset is
// slot * dim^3 in both. shaders/field.glslh reads them through FIELD_VOLUME_SDF_BINDING and
// FIELD_VOLUME_MAT_BINDING.
//
// The CPU's ve::VolumeSet is authoritative (see the plan's Deliberate Decisions); this class
// only ever receives uploads. There is one instance per device: GpuAtlas owns the render
// device's, MeshPass owns the mesher worker's.
class VolumePool {
public:
	~VolumePool();

	bool initialize(RenderingDevice *rd, int slots, int dim);
	void teardown();
	bool is_valid() const { return sdf_.is_valid() && mat_.is_valid(); }

	RID sdf_buffer() const { return sdf_; }
	RID mat_buffer() const { return mat_; }
	int slots() const { return slots_; }
	int dim() const { return dim_; }

	// Copies one slot's bytes to the device. A device-level command: record it BEFORE
	// compute_list_begin, never inside an open list (M2 Task 12's ordering rule).
	// Rejects a mismatched dim rather than writing a torn lattice.
	bool upload(RenderingDevice *rd, int slot, const ve::VolumeData &data);

private:
	RenderingDevice *rd_ = nullptr;
	RID sdf_, mat_;
	int slots_ = 0, dim_ = 0;
};

} // namespace godot
