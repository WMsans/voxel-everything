#include "render/field_context_set.h"
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

FieldContextSet::~FieldContextSet() { teardown(); }

bool FieldContextSet::initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p) {
	teardown();
	rd_ = rd;
	if (rd == nullptr || !shader.is_valid()) return false;

	// std140 pads every scalar to 16 bytes. The generated UBO declares one scalar per param
	// in pipeline order, so the CPU-side packing must pad identically. A pipeline with no
	// params still gets one vec4 (the generated `_unused`), so the buffer is never empty --
	// RenderingDevice rejects a zero-byte uniform buffer.
	PackedByteArray bytes;
	const int count = p.params.empty() ? 4 : int(p.params.size()) * 4;
	bytes.resize(count * 4);
	bytes.fill(0);
	for (size_t i = 0; i < p.params.size(); i++)
		bytes.encode_float(int64_t(i) * 16, p.params[i].value);
	params_ubo_ = rd->uniform_buffer_create(bytes.size(), bytes);
	if (!params_ubo_.is_valid()) return false;

	// Plan A ships an empty sector map: one int, value -1, meaning "no sector resident", so
	// sample_sector_* returns its declared fallback everywhere. Plan B replaces this.
	PackedByteArray empty;
	empty.resize(4);
	empty.fill(0);
	empty.encode_s32(0, -1);
	sector_map_ = rd->storage_buffer_create(empty.size(), empty);
	if (!sector_map_.is_valid()) { teardown(); return false; }

	Ref<RDUniform> u0;
	u0.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_UNIFORM_BUFFER);
	u0->set_binding(0);
	u0->add_id(params_ubo_);

	Ref<RDUniform> u1;
	u1.instantiate();
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_STORAGE_BUFFER);
	u1->set_binding(1);
	u1->add_id(sector_map_);

	// Plan A resolves no sampled resources; resolve_pipeline() accepts them but no shipped
	// stage declares one. Assert rather than silently binding an incomplete set.
	if (!p.resources.empty()) { teardown(); return false; }

	uset_ = rd->uniform_set_create(Array::make(u0, u1), shader, 1);
	if (!uset_.is_valid()) { teardown(); return false; }
	return true;
}

void FieldContextSet::teardown() {
	if (rd_ == nullptr) return;
	if (uset_.is_valid()) { rd_->free_rid(uset_); uset_ = RID(); }
	if (sector_map_.is_valid()) { rd_->free_rid(sector_map_); sector_map_ = RID(); }
	if (params_ubo_.is_valid()) { rd_->free_rid(params_ubo_); params_ubo_ = RID(); }
	rd_ = nullptr;
}

void FieldContextSet::bind(RenderingDevice *rd, int64_t list) const {
	if (uset_.is_valid()) rd->compute_list_bind_uniform_set(list, uset_, 1);
}
