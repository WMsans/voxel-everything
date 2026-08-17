#pragma once
#include "mesh/mesh_chunk.h"
#include "world/region.h"
#include <map>
#include <vector>

namespace ve {

// How the residency asks whether a chunk is worth meshing. An interface for the same reason
// ve::Generator is one: the real implementation needs the generator AND the edit log, and the
// edit log's lock lives on the Godot side of the wall.
struct ChunkProbe {
	virtual ~ChunkProbe() = default;
	virtual bool chunk_has_surface(IVec3 chunk) const = 0;
};

struct ChunkResidencyConfig {
	WorldBounds bounds{};
	float radius_m = 64.0f;   // spec §6: collision streams in a ~64 m radius
	// Eight times the old 160, because a 6.4 m chunk covers an eighth of the volume the
	// 12.8 m one did; this is the same ball, parcelled finer.
	int max_chunks = 1280;
	int max_builds_per_frame = 2;
	// One probe is 125 field evaluations and a fresh world sees ~10 000 unknown chunks on its
	// first frame. Budgeted, nearest first: the ground under the player resolves immediately
	// and the edge of the ball catches up over the next few frames.
	int max_probes_per_frame = 256;
	// A chunk is released only past radius_m * evict_margin. Without the gap a player standing
	// exactly on the boundary would mesh and drop the same chunk every frame.
	float evict_margin = 1.15f;
};

struct ChunkPlan {
	struct Entry {
		IVec3 chunk;
		int slot = -1;
	};
	std::vector<Entry> builds;   // slot reserved; the caller meshes these
	std::vector<Entry> releases; // slot freed; the caller drops the collider
};

// Which collision chunks are resident, in which pool slot, and which of them still owe a
// mesh. Distance-LRU: when the pool is full a closer candidate displaces the furthest
// resident, exactly as ve::RegionResidency does for region slots.
class ChunkResidency {
public:
	enum State { kNeedsBuild = 0, kBuilding = 1, kReady = 2 };

	explicit ChunkResidency(const ChunkResidencyConfig &cfg);

	// centers holds 3 floats per centre; radii holds one per centre, or nullptr to use
	// cfg.radius_m for all of them. Spec §6 wants "a ~64 m radius around the player + small
	// bubbles around active bodies"; M3 passes the player alone, M4's islands pass more.
	// max_builds clamps cfg.max_builds_per_frame downwards for this frame (negative = use
	// the config); the caller passes 0 while the mesher still has a batch in flight.
	ChunkPlan update(const float *centers, const float *radii, int center_count,
			const ChunkProbe &probe, int max_builds = -1);

	// Every chunk in the inclusive range needs its collider rebuilt, and every cached probe
	// verdict inside it is dropped: an op can put a surface into a chunk that had none, and
	// a cached "empty" would hide it for ever.
	void mark_dirty(IVec3 lo, IVec3 hi);

	void note_built(IVec3 chunk);  // a collider now exists for this chunk
	void note_failed(IVec3 chunk); // build failed: keep the slot, retry next frame
	int note_empty(IVec3 chunk);   // no geometry: caches "empty" when the build is still current, frees and RETURNS the slot
	void note_discarded(IVec3 chunk); // result landed after eviction/displacement: clear in-flight only

	int slot_of(IVec3 chunk) const;
	// Diagnostic only: the chunk's residency State, or -1 when not resident, and whether an
	// outstanding build exists for it. Used by tests diagnosing stale colliders.
	int slot_state_of(IVec3 chunk) const;
	bool build_in_flight(IVec3 chunk) const;
	IVec3 chunk_of_slot(int slot) const;
	bool slot_resident(int slot) const;
	int resident_count() const { return static_cast<int>(by_chunk_.size()); }
	int pending_count() const; // resident but not yet built
	int probe_cache_size() const { return static_cast<int>(probe_cache_.size()); }
	void clear();
	const ChunkResidencyConfig &config() const { return cfg_; }

private:
	struct Key {
		int x, y, z;
		bool operator<(const Key &o) const {
			if (z != o.z) return z < o.z;
			if (y != o.y) return y < o.y;
			return x < o.x;
		}
	};
	static Key key(IVec3 c) { return Key{c.x, c.y, c.z}; }
	void release(IVec3 chunk, int slot, ChunkPlan *plan);

	ChunkResidencyConfig cfg_;
	std::map<Key, int> by_chunk_;    // chunk -> slot
	std::vector<IVec3> slot_chunk_;  // slot -> chunk (valid where slot_used_)
	std::vector<char> slot_used_;
	std::vector<char> slot_state_;   // State
	std::vector<int> free_slots_;
	std::map<Key, char> probe_cache_; // 1 = may hold a surface, 0 = known empty
	std::map<Key, char> in_flight_;   // released while kBuilding; result still outstanding
};

} // namespace ve
