#include "render/sector_context.h"
#include "gpu_layout/blocks.h"
#include "render/field_context_set.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cstring>

using namespace godot;

SectorContext::~SectorContext() { teardown(); }

bool SectorContext::initialize(RenderingDevice *rd, std::shared_ptr<ve::SectorCache> cache) {
	teardown();
	rd_ = rd;
	cache_ = std::move(cache);
	if (rd == nullptr || !cache_) return false;
	program_ = gpu::compile_compute(rd, group_, "SectorContext", "sector_bake.comp.glsl");
	if (!program_.valid()) return false;
	// field_ops.glslh's op pool, which the bake never reads but the set must cover.
	PackedByteArray zeros;
	zeros.resize(32);
	zeros.fill(0);
	op_dummy_ = rd->storage_buffer_create(zeros.size(), zeros);
	for (Bake &b : ring_) {
		b.buffer = rd->storage_buffer_create(kBakeBytes);
		b.read.instantiate();
		if (!b.buffer.is_valid()) return false;
	}
	return op_dummy_.is_valid() && mirror_.initialize(rd, cache_->max_resident());
}

void SectorContext::teardown() {
	if (rd_ != nullptr) {
		// RenderingDevice has no cancellation: flush each outstanding read while its buffer
		// still exists (AsyncBufferRead::drain's contract).
		for (Bake &b : ring_) {
			if (b.busy && b.read.is_valid()) b.read->drain(rd_);
			if (b.buffer.is_valid()) rd_->free_rid(b.buffer);
			b = Bake();
		}
		if (op_dummy_.is_valid()) rd_->free_rid(op_dummy_);
		gpu::RdDevice d{rd_};
		group_.release(d);
	}
	op_dummy_ = RID();
	program_ = gpu::Program();
	mirror_.teardown();
	rd_ = nullptr;
	cache_.reset();
	dispatched_ = 0;
}

void SectorContext::run_frame(RenderingDevice *rd, float cam_x, float cam_z,
		const FieldContextSet *field) {
	if (rd == nullptr || rd != rd_ || !program_.valid() || !cache_) return;
	for (Bake &b : ring_) {
		if (!b.busy || !b.read->take_fresh()) continue;
		b.busy = false;
		const PackedByteArray &d = b.read->data();
		auto t = std::make_shared<ve::SectorTexels>();
		t->texels.resize(size_t(ve::kSectorStride * ve::kSectorStride));
		if (d.size() < kBakeBytes) {
			cache_->abandon(b.c);
			continue;
		}
		std::memcpy(t->texels.data(), d.ptr(), size_t(kBakeBytes));
		t->max_slope = ve::sector_max_axis_slope(*t);
		if (t->max_slope > ve::kSectorSlopeLimit)
			UtilityFunctions::push_warning(String("sector (") + String::num_int64(b.c.x) + ", " +
					String::num_int64(b.c.z) + ") is steeper than the field bound assumes: " +
					String::num(t->max_slope, 2) + " > " + String::num(ve::kSectorSlopeLimit, 2));
		cache_->insert(b.c, std::move(t));
	}
	int free = 0;
	for (const Bake &b : ring_) free += b.busy ? 0 : 1;
	const ve::SectorCache::Plan plan = cache_->plan(cam_x, cam_z, std::min(free, bakes_per_frame_));
	gpu::RdDevice device{rd};
	for (const ve::SectorCoord &c : plan.bake) {
		Bake *slot = nullptr;
		for (Bake &b : ring_)
			if (!b.busy) { slot = &b; break; }
		const RID set = (slot && field && field->is_valid())
				? slot->set.get(device, group_, program_.shader, 0,
						{gpu::storage(0, slot->buffer), gpu::storage(1, op_dummy_)})
				: RID();
		const ve::SectorBakePush push{{c.x, c.z, 0, 0}};
		const bool ok = set.is_valid() &&
				gpu::dispatch(rd, program_.pipeline, {{set, 0}, {field->uniform_set(), 1}},
						gpu::push_bytes(push), gpu::groups(ve::kSectorStride, 8),
						gpu::groups(ve::kSectorStride, 8)) &&
				slot->read->request(rd, slot->buffer, 0, uint32_t(kBakeBytes));
		if (!ok) {
			cache_->abandon(c);
			continue;
		}
		slot->c = c;
		slot->busy = true;
		dispatched_++;
	}
	mirror_.sync(*cache_);
}

bool SectorContext::ready_on_render(float min_x, float min_z, float max_x, float max_z) const {
	for (const ve::SectorCoord &c : cache_->needed_world(min_x, min_z, max_x, max_z))
		if (!mirror_.has(c)) return false;
	return true;
}

bool SectorContext::ready_on_host(float min_x, float min_z, float max_x, float max_z) const {
	return cache_->ready_world(min_x, min_z, max_x, max_z);
}
