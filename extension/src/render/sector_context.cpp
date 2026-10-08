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
	if (!op_dummy_.is_valid() || !mirror_.initialize(rd, cache_->max_resident())) return false;
	// Devices address sectors only through the toroidal window, and sector_window writes
	// cells at most kSectorWindow / 2 - 1 from the camera's sector. In the cache's own
	// point-to-square metric, a sector 12 cells out sits strictly more than 11 sectors
	// from any camera inside the centre sector, so a demand radius beyond that reach
	// gates streaming on sectors that can never appear in a device's window: terrain
	// there reads the window's empty cell forever, with no diagnostic. stream_radius_m
	// is user-settable (core/world_store.h); report once per graph init rather than
	// clamp silently -- the radius also sizes the sun cascades and far-field relief,
	// and a tier-local clamp would fork this world away from them.
	const float reach_m = float(ve::kSectorWindow / 2 - 1) * ve::kSectorSizeM;
	if (cache_->radius_m() > reach_m) {
		UtilityFunctions::printerr(String("SectorContext: stream_radius_m ") +
				String::num(cache_->radius_m(), 1) + " m exceeds the sector window's reach of " +
				String::num(reach_m, 1) + " m; sectors farther than that can never appear in a "
				"device's window, so map-stage terrain there streams on the fallback height "
				"forever. Lower stream_radius_m (the shipped default is 4000 m).");
	}
	return true;
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
		const bool over = t->max_slope > ve::kSectorSlopeLimit;
		const float slope = t->max_slope;
		cache_->insert(b.c, std::move(t));
		// Spec §5.5: the over-limit case is logged once, counted in the stats. This
		// harvest loop is render-thread-only and insert() is the count's only writer,
		// so over_limit == 1 right after this insert is the 0→1 transition.
		if (over && cache_->stats().over_limit == 1)
			UtilityFunctions::push_warning(String("sector (") + String::num_int64(b.c.x) + ", " +
					String::num_int64(b.c.z) + ") is steeper than the field bound assumes: " +
					String::num(slope, 2) + " > " + String::num(ve::kSectorSlopeLimit, 2) +
					" (further over-limit sectors are counted in the sector stats only)");
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
