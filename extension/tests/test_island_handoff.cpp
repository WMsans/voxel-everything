#include <doctest/doctest.h>
#include "render/island_handoff.h"
#include <thread>
#include <vector>

using godot::IslandHandoff;
using godot::IslandSlotDesc;

namespace {
ve::VolumeData volume(uint8_t fill, int dim = 2) {
	ve::VolumeData d;
	d.dim = dim;
	d.sdf.assign(static_cast<size_t>(dim * dim * dim), fill);
	d.mat.assign(static_cast<size_t>(dim * dim * dim), 1);
	return d;
}
} // namespace

TEST_CASE("island handoff: take returns uploads in queue order and empties the queue") {
	IslandHandoff h;
	h.queue_island(3, 7, volume(10));
	h.queue_field_volume(9, volume(20));
	CHECK(h.pending_uploads() == 2);
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.uploads.size() == 2);
	CHECK(b.uploads[0].atlas_slot == 3);
	CHECK(b.uploads[0].volume_slot == 7);
	CHECK(b.uploads[0].to_island_atlas);
	CHECK(b.uploads[1].atlas_slot == -1);
	CHECK(b.uploads[1].volume_slot == 9);
	CHECK_FALSE(b.uploads[1].to_island_atlas);
	CHECK(b.uploads[1].data.sdf[0] == 20);
	CHECK(h.pending_uploads() == 0);
	CHECK(h.take().uploads.empty());
}

TEST_CASE("island handoff: discard removes only field-volume uploads for that slot") {
	IslandHandoff h;
	h.queue_island(0, 4, volume(1));
	h.queue_field_volume(4, volume(2));
	h.queue_field_volume(5, volume(3));
	h.discard_field_volume(4);
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.uploads.size() == 2);
	CHECK(b.uploads[0].to_island_atlas);
	CHECK(b.uploads[0].volume_slot == 4);
	CHECK(b.uploads[1].volume_slot == 5);
}

TEST_CASE("island handoff: the dirty flag is consumed by one take; the descriptors are always handed over") {
	IslandHandoff h;
	CHECK_FALSE(h.descs_dirty());
	std::vector<IslandSlotDesc> d(2);
	d[1].live = true;
	h.publish_descriptors(d);
	CHECK(h.descs_dirty());
	IslandHandoff::Batch first = h.take();
	CHECK(first.descs_dirty);
	REQUIRE(first.descs.size() == 2);
	CHECK(first.descs[1].live);
	CHECK_FALSE(h.descs_dirty());
	IslandHandoff::Batch second = h.take();
	CHECK_FALSE(second.descs_dirty);
	CHECK(second.descs.size() == 2); // the last published set is copied every drain, as before
}

TEST_CASE("island handoff: physics teardown keeps pinned field volumes and drops the rest") {
	ve::VolumeSet volumes;
	REQUIRE(volumes.reserve(2));
	REQUIRE(volumes.store(2, volume(5)));
	REQUIRE(volumes.pin(2));
	REQUIRE(volumes.reserve(3));
	REQUIRE(volumes.store(3, volume(6))); // stored but not pinned
	IslandHandoff h;
	h.queue_field_volume(2, volume(5));
	h.queue_field_volume(3, volume(6));
	h.queue_island(0, 2, volume(5)); // island uploads never survive physics teardown
	h.publish_descriptors(std::vector<IslandSlotDesc>(1));
	h.drop_for_physics_teardown(volumes);
	CHECK_FALSE(h.descs_dirty());
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.uploads.size() == 1);
	CHECK(b.uploads[0].volume_slot == 2);
	CHECK_FALSE(b.uploads[0].to_island_atlas);
	CHECK(b.descs.empty());
}

TEST_CASE("island handoff: slot count is the larger high-water mark, zero when islands are off") {
	IslandHandoff h;
	CHECK(h.slot_count(true) == 0);
	h.manager_slots.store(3);
	h.note_debug_slot(1); // slot 1 -> mark 2
	CHECK(h.slot_count(true) == 3);
	h.note_debug_slot(5); // -> 6
	CHECK(h.slot_count(true) == 6);
	h.note_debug_slot(0); // a lower slot never lowers the mark
	CHECK(h.slot_count(true) == 6);
	CHECK(h.slot_count(false) == 0);
	h.reset_debug_slots();
	CHECK(h.slot_count(true) == 3);
}

TEST_CASE("release_volume_slot queues a normal release only when the slot was freed") {
	ve::VolumeSet volumes;
	REQUIRE(volumes.reserve(1));
	REQUIRE(volumes.store(1, volume(9)));
	REQUIRE(volumes.reserve(2));
	REQUIRE(volumes.store(2, volume(9)));
	REQUIRE(volumes.pin(2));
	IslandHandoff h;
	CHECK(godot::release_volume_slot(volumes, h, 1));
	CHECK_FALSE(godot::release_volume_slot(volumes, h, 2)); // pinned: refused, normals kept
	CHECK_FALSE(godot::release_volume_slot(volumes, h, 1)); // already free
	IslandHandoff::Batch b = h.take();
	REQUIRE(b.normal_releases.size() == 1);
	CHECK(b.normal_releases[0] == 1);
}

TEST_CASE("island handoff: a producer thread and a draining thread lose nothing") {
	IslandHandoff h;
	const size_t kCount = 2000;
	std::thread producer([&] {
		for (size_t i = 0; i < kCount; i++) h.queue_field_volume(static_cast<int>(i % 64), volume(1));
	});
	size_t taken = 0;
	while (taken < kCount) taken += h.take().uploads.size();
	producer.join();
	taken += h.take().uploads.size();
	CHECK(taken == kCount);
}
