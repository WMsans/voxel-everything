#include <doctest/doctest.h>
#include "world/normal_range_allocator.h"
#include <vector>

using namespace ve;

TEST_CASE("exact-fit allocation fills the pool and refuses more") {
    NormalRangeAllocator a(1000);
    CHECK(a.used_bytes() == 0);
    NormalAllocation x = a.allocate(1000);
    CHECK(x.valid());
    CHECK(x.offset == 0);
    CHECK(x.size == 1000);
    CHECK(x.generation != 0);
    CHECK(a.used_bytes() == 1000);
    CHECK(a.high_water_bytes() == 1000);
    NormalAllocation y = a.allocate(4);
    CHECK_FALSE(y.valid());
    // A failed allocation must not move the counters.
    CHECK(a.used_bytes() == 1000);
}

TEST_CASE("sizes round up to four bytes") {
    NormalRangeAllocator a(64);
    NormalAllocation x = a.allocate(1);
    CHECK(x.valid());
    CHECK(x.size == 4);
    NormalAllocation y = a.allocate(3);
    CHECK(y.valid());
    CHECK(y.offset == 4);
    CHECK(y.size == 4);
}

TEST_CASE("aligned requests split a prefix fragment and stay aligned") {
    NormalRangeAllocator a(128);
    NormalAllocation x = a.allocate(4, 16);   // offset 0, size 4
    REQUIRE(x.valid());
    CHECK(x.offset == 0);
    NormalAllocation y = a.allocate(4, 16);   // first fit at 4, aligned up to 16
    REQUIRE(y.valid());
    CHECK(y.offset == 16);
    // The prefix [4,16) is free and usable by an unaligned request.
    NormalAllocation z = a.allocate(8);
    REQUIRE(z.valid());
    CHECK(z.offset == 4);
    CHECK(z.size == 8);
}

TEST_CASE("adjacent free blocks coalesce on release") {
    NormalRangeAllocator a(48);
    NormalAllocation p = a.allocate(16);
    NormalAllocation q = a.allocate(16);
    NormalAllocation r = a.allocate(16);
    REQUIRE(p.valid());
    REQUIRE(q.valid());
    REQUIRE(r.valid());
    CHECK(p.offset == 0);
    CHECK(q.offset == 16);
    CHECK(r.offset == 32);

    // Free the two ends first, then the middle: all three ranges touch, so the
    // allocator must end with ONE free block spanning the whole pool.
    CHECK(a.release(p));
    CHECK(a.release(r));
    CHECK(a.used_bytes() == 16);
    CHECK(a.release(q));
    CHECK(a.used_bytes() == 0);

    NormalAllocation whole = a.allocate(44);
    REQUIRE(whole.valid());
    CHECK(whole.offset == 0);
    CHECK(whole.size == 44);
}

TEST_CASE("a freed span is reused and its generation increments") {
    NormalRangeAllocator a(32);
    NormalAllocation x = a.allocate(16);
    REQUIRE(x.valid());
    CHECK(x.generation != 0);
    CHECK(a.release(x));

    NormalAllocation y = a.allocate(16);
    REQUIRE(y.valid());
    CHECK(y.offset == x.offset); // reused the same span
    CHECK(y.generation == x.generation + 1);

    // And once more: generations keep climbing for the same offset.
    CHECK(a.release(y));
    NormalAllocation z = a.allocate(16);
    REQUIRE(z.valid());
    CHECK(z.offset == x.offset);
    CHECK(z.generation == x.generation + 2);
}

TEST_CASE("exhaustion returns invalid allocations without state changes") {
    NormalRangeAllocator a(24);
    std::vector<NormalAllocation> live;
    for (;;) {
        NormalAllocation x = a.allocate(8);
        if (!x.valid()) break;
        live.push_back(x);
    }
    CHECK(live.size() == 3);
    CHECK(a.used_bytes() == 24);
    CHECK(a.high_water_bytes() == 24);
}

TEST_CASE("double-free is rejected and leaves state unchanged") {
    NormalRangeAllocator a(32);
    NormalAllocation x = a.allocate(16);
    REQUIRE(x.valid());
    CHECK(a.release(x));
    CHECK(a.used_bytes() == 0);
    const uint32_t gen_after_first_free = 0;
    (void)gen_after_first_free;
    CHECK_FALSE(a.release(x)); // second release of the same handle
    CHECK(a.used_bytes() == 0);

    // The allocator still works exactly as before.
    NormalAllocation y = a.allocate(32);
    REQUIRE(y.valid());
    CHECK(y.offset == 0);
    CHECK(y.size == 32);
    CHECK(a.used_bytes() == 32);
}

TEST_CASE("stale-generation handles are rejected") {
    NormalRangeAllocator a(32);
    NormalAllocation stale = a.allocate(16);
    REQUIRE(stale.valid());
    CHECK(a.release(stale));
    NormalAllocation fresh = a.allocate(16); // same offset, newer generation
    REQUIRE(fresh.valid());
    CHECK(fresh.offset == stale.offset);
    CHECK(fresh.generation > stale.generation);

    // Releasing the OLD handle must fail and must not disturb the live one.
    CHECK_FALSE(a.release(stale));
    CHECK(a.used_bytes() == 16);
    CHECK(a.release(fresh));
    CHECK(a.used_bytes() == 0);
}

TEST_CASE("size-mismatched handles are rejected") {
    NormalRangeAllocator a(32);
    NormalAllocation x = a.allocate(16);
    REQUIRE(x.valid());
    NormalAllocation forged = x;
    forged.size = 32; // lie about the size
    CHECK_FALSE(a.release(forged));
    CHECK(a.used_bytes() == 16);
    CHECK(a.release(x));
    CHECK(a.used_bytes() == 0);
}

TEST_CASE("zero-capacity and zero-size allocations are rejected") {
    NormalRangeAllocator a;
    CHECK_FALSE(a.allocate(4).valid());
    NormalRangeAllocator b(16);
    CHECK_FALSE(b.allocate(0).valid());
    NormalAllocation bogus; // default handle
    CHECK_FALSE(bogus.valid());
    CHECK_FALSE(b.release(bogus));
}
