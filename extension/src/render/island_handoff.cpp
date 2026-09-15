#include "render/island_handoff.h"
#include <algorithm>

namespace godot {

void IslandSlotDesc::recompute_world_aabb() {
	const float span = static_cast<float>(dim - 1) * voxel;
	for (int a = 0; a < 3; a++) {
		aabb_lo[a] = 1e30f;
		aabb_hi[a] = -1e30f;
	}
	for (int c = 0; c < 8; c++) {
		const float q[3] = {lattice_origin[0] + ((c & 1) ? span : 0.0f),
				lattice_origin[1] + ((c & 2) ? span : 0.0f),
				lattice_origin[2] + ((c & 4) ? span : 0.0f)};
		for (int a = 0; a < 3; a++) {
			// basis is COLUMN major: world_a = sum_k basis[k * 3 + a] * q[k].
			const float w = basis[0 * 3 + a] * q[0] + basis[1 * 3 + a] * q[1] +
					basis[2 * 3 + a] * q[2] + origin[a];
			aabb_lo[a] = std::min(aabb_lo[a], w);
			aabb_hi[a] = std::max(aabb_hi[a], w);
		}
	}
}

void IslandHandoff::queue_island(int atlas_slot, int volume_slot, const ve::VolumeData &data) {
	std::lock_guard<std::mutex> lock(mutex_);
	uploads_.push_back(Upload{atlas_slot, volume_slot, true, data});
}

void IslandHandoff::queue_field_volume(int volume_slot, const ve::VolumeData &data) {
	std::lock_guard<std::mutex> lock(mutex_);
	uploads_.push_back(Upload{-1, volume_slot, false, data});
}

void IslandHandoff::discard_field_volume(int volume_slot) {
	std::lock_guard<std::mutex> lock(mutex_);
	uploads_.erase(std::remove_if(uploads_.begin(), uploads_.end(),
						   [volume_slot](const Upload &u) {
							   return !u.to_island_atlas && u.volume_slot == volume_slot;
						   }),
				uploads_.end());
}

void IslandHandoff::queue_normal_release(int volume_slot) {
	std::lock_guard<std::mutex> lock(mutex_);
	normal_releases_.push_back(volume_slot);
}

void IslandHandoff::publish_descriptors(std::vector<IslandSlotDesc> descs) {
	std::lock_guard<std::mutex> lock(mutex_);
	descs_ = std::move(descs);
	descs_dirty_ = true;
}

IslandHandoff::Batch IslandHandoff::take() {
	Batch b;
	std::lock_guard<std::mutex> lock(mutex_);
	b.uploads.swap(uploads_);
	b.normal_releases.swap(normal_releases_);
	b.descs = descs_;
	b.descs_dirty = descs_dirty_;
	descs_dirty_ = false;
	return b;
}

void IslandHandoff::drop_for_physics_teardown(const ve::VolumeSet &volumes) {
	std::lock_guard<std::mutex> lock(mutex_);
	std::vector<Upload> keep;
	keep.reserve(uploads_.size());
	for (Upload &u : uploads_)
		if (!u.to_island_atlas && volumes.pinned(u.volume_slot)) keep.push_back(std::move(u));
	uploads_.swap(keep);
	descs_.clear();
	descs_dirty_ = false;
}

void IslandHandoff::note_debug_slot(int slot) {
	int current = debug_slots_.load(std::memory_order_relaxed);
	while (slot + 1 > current &&
			!debug_slots_.compare_exchange_weak(current, slot + 1, std::memory_order_relaxed)) {
	}
}

void IslandHandoff::reset_debug_slots() {
	debug_slots_.store(0, std::memory_order_relaxed);
}

int IslandHandoff::slot_count(bool islands_enabled) const {
	if (!islands_enabled) return 0;
	const int debug = debug_slots_.load(std::memory_order_relaxed);
	const int manager = manager_slots.load(std::memory_order_relaxed);
	return debug > manager ? debug : manager;
}

int IslandHandoff::pending_uploads() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return static_cast<int>(uploads_.size());
}

bool IslandHandoff::descs_dirty() const {
	std::lock_guard<std::mutex> lock(mutex_);
	return descs_dirty_;
}

int IslandHandoff::field_volume_uploads() const {
	return field_volume_uploads_.load(std::memory_order_relaxed);
}

void IslandHandoff::note_field_volume_uploaded() {
	field_volume_uploads_.fetch_add(1, std::memory_order_relaxed);
}

bool release_volume_slot(ve::VolumeSet &volumes, IslandHandoff &handoff, int slot) {
	// The authoritative copy goes first; only a successful release (never a pinned slot --
	// a pasted volume-add still names it) queues the GPU-side normal teardown.
	const bool freed = volumes.release(slot);
	if (freed) handoff.queue_normal_release(slot);
	return freed;
}

} // namespace godot
