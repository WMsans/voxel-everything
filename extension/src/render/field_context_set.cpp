#include "render/field_context_set.h"
#include "terrain/field_params_pack.h"
#include <cstring>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

using namespace godot;

FieldContextSet::~FieldContextSet() { teardown(); }

bool FieldContextSet::initialize(RenderingDevice *rd, RID shader, const ve::ResolvedPipeline &p) {
	teardown();
	rd_ = rd;
	if (rd == nullptr || !shader.is_valid()) return false;

	// Params packing: values are DENSE (one float per 4 bytes, in pipeline order) in a
	// buffer sized to Godot's 16-byte-stride expectation (16 bytes per param). See
	// ve::pack_field_params_bytes for why both halves are load-bearing. A pipeline with
	// no params still gets one vec4 (the generated `_unused`), so the buffer is never
	// empty -- RenderingDevice rejects a zero-byte uniform buffer. The differential
	// probe/engine suites pin the values end to end; touch this only if they go red together.
	const std::vector<uint8_t> packed = ve::pack_field_params_bytes(p);
	PackedByteArray bytes;
	bytes.resize(int(packed.size()));
	if (!packed.empty())
		std::memcpy(bytes.ptrw(), packed.data(), packed.size());
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
