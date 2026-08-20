#pragma once
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <vector>
#include "world/override_store.h"

namespace godot {

class OverridePool {
public:
	static constexpr int kMaxOverrideTables = 32;
	static constexpr int kDefaultCapacity = 8192;
	static constexpr int kDefaultRegionSlots = 512;

	OverridePool() = default;
	~OverridePool();
	OverridePool(const OverridePool &) = delete;
	OverridePool &operator=(const OverridePool &) = delete;

	bool initialize(RenderingDevice *rd, int capacity, int max_region_slots = kDefaultRegionSlots);
	void teardown();
	bool is_valid() const { return sdf_.is_valid() && mat_.is_valid() && tables_.is_valid() && region_map_.is_valid(); }
	int capacity() const { return capacity_; }
	int max_region_slots() const { return max_region_slots_; }

	bool upload(int slot, const ve::OverrideBrick &brick);
	void set_table_entry(RenderingDevice *rd, int table, int brick_index, int slot);
	void set_region_table(RenderingDevice *rd, int region_slot, int table);
	int region_table(int region_slot) const;
	void clear_table(RenderingDevice *rd, int table);

	RID sdf_buffer() const { return sdf_; }
	RID mat_buffer() const { return mat_; }
	RID tables() const { return tables_; }
	RID region_table_map() const { return region_map_; }

private:
	RenderingDevice *rd_ = nullptr;
	RID sdf_, mat_, tables_, region_map_;
	int capacity_ = 0;
	int max_region_slots_ = 0;
	std::vector<int> region_tables_;
};

} // namespace godot
