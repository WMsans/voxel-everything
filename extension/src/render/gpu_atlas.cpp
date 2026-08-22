#include <algorithm>
#include <cstring>
#include "render/gpu_atlas.h"
#include <godot_cpp/classes/rd_texture_format.hpp>
#include <godot_cpp/classes/rd_texture_view.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <vector>

using namespace godot;

static_assert(ve::kOccupancyBlockBytes == ve::kRegionBrickCount / 4,
		"update OCC_WORDS_PER_REGION in shaders/brick_mark.comp.glsl and region_free.comp.glsl");

namespace {

void free_if_valid(RenderingDevice *rd, RID &rid) {
	if (rd && rid.is_valid()) rd->free_rid(rid);
	rid = RID();
}

PackedByteArray filled_i32(int count, int32_t value) {
	PackedByteArray b;
	b.resize(static_cast<int64_t>(count) * 4);
	int32_t *p = reinterpret_cast<int32_t *>(b.ptrw());
	for (int i = 0; i < count; i++) p[i] = value;
	return b;
}

PackedByteArray zeroed(int64_t bytes) {
	PackedByteArray b;
	b.resize(bytes);
	b.fill(0);
	return b;
}

} // namespace

GpuAtlas::~GpuAtlas() {
	teardown();
}

RID GpuAtlas::make_volume(RenderingDevice *rd, RenderingDevice::DataFormat fmt, int w, int h,
		int d) {
	Ref<RDTextureFormat> f;
	f.instantiate();
	f->set_texture_type(RenderingDevice::TEXTURE_TYPE_3D);
	f->set_format(fmt);
	f->set_width(w);
	f->set_height(h);
	f->set_depth(d);
	f->set_mipmaps(1);
	// STORAGE is what lets brick_gen.comp.glsl write these volumes; SAMPLING is what lets
	// the raymarcher read them; CAN_COPY_FROM is what lets the differential test read them
	// back. CAN_UPDATE stays for a future CPU-side override path.
	f->set_usage_bits(RenderingDevice::TEXTURE_USAGE_SAMPLING_BIT |
			RenderingDevice::TEXTURE_USAGE_STORAGE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_UPDATE_BIT |
			RenderingDevice::TEXTURE_USAGE_CAN_COPY_FROM_BIT);
	Ref<RDTextureView> v;
	v.instantiate();
	return rd->texture_create(f, v, TypedArray<PackedByteArray>());
}

bool GpuAtlas::initialize(RenderingDevice *rd, const GpuAtlasConfig &cfg) {
	teardown();
	rd_ = rd;
	cfg_ = cfg;
	if (!rd) return false;
	const int slots = atlas_slot_count();
	if (slots <= 0 || cfg_.max_region_slots <= 0 || cfg_.max_brick_jobs <= 0 ||
			cfg_.max_override_bricks <= 0) {
		UtilityFunctions::printerr("GpuAtlas: degenerate configuration");
		return false;
	}

	const ve::IVec3 ab = cfg_.atlas_bricks;
	sdf_atlas_ = make_volume(rd, RenderingDevice::DATA_FORMAT_R8_UNORM,
			ab.x * ve::kBrickSdfStride, ab.y * ve::kBrickSdfStride, ab.z * ve::kBrickSdfStride);
	mat_atlas_ = make_volume(rd, RenderingDevice::DATA_FORMAT_R8_UINT,
			ab.x * ve::kBrickVoxels, ab.y * ve::kBrickVoxels, ab.z * ve::kBrickVoxels);
	for (int l = 0; l < ve::kMipLevels; l++) {
		const int d = ve::kMipDims[l];
		mips_[l] = make_volume(rd, RenderingDevice::DATA_FORMAT_R8G8_UINT, ab.x * d, ab.y * d,
				ab.z * d);
	}

	// Palette entries are uint32, not the uint16 of ve::Brick. A packed uint16 array would
	// force GL_EXT_shader_16bit_storage on every consumer to save 512 KB; it is not worth it.
	palette_ = rd->storage_buffer_create(static_cast<uint32_t>(slots) * ve::kBrickPaletteSize * 4,
			zeroed(static_cast<int64_t>(slots) * ve::kBrickPaletteSize * 4));
	// Every never-generated slot must be conservative: zero means "skip" to the marcher, and
	// the atlas contains stale bytes until brick_gen overwrites a slot.
	brick_flags_ = rd->storage_buffer_create(static_cast<uint32_t>(slots) * sizeof(uint32_t),
			filled_i32(slots, static_cast<int32_t>(ve::kBrickFlagConservative)));

	// The region map is a BUFFER, not a texture: streaming re-points one entry per region,
	// and buffer_update writes four bytes where texture_update would rewrite a whole layer.
	region_map_ = rd->storage_buffer_create(static_cast<uint32_t>(region_map_entries()) * 4,
			filled_i32(region_map_entries(), -1));

	const int64_t table_entries =
			static_cast<int64_t>(cfg_.max_region_slots) * ve::kRegionBrickCount;
	{
		// Built in one go on the CPU: 64 MB at the shipping configuration, once at startup.
		PackedByteArray absent = filled_i32(static_cast<int>(table_entries), -1);
		region_tables_ = rd->storage_buffer_create(static_cast<uint32_t>(absent.size()), absent);
	}

	{
		PackedByteArray fl;
		fl.resize(static_cast<int64_t>(slots) * 4);
		int32_t *p = reinterpret_cast<int32_t *>(fl.ptrw());
		for (int i = 0; i < slots; i++) p[i] = i;
		free_list_ = rd->storage_buffer_create(static_cast<uint32_t>(fl.size()), fl);
	}
	{
		PackedByteArray c = zeroed(16);
		reinterpret_cast<int32_t *>(c.ptrw())[0] = slots; // free_count
		counters_ = rd->storage_buffer_create(16, c);
	}
	frame_ = rd->storage_buffer_create(16, zeroed(16));
	dispatch_args_ = rd->storage_buffer_create(16, zeroed(16),
			RenderingDevice::STORAGE_BUFFER_USAGE_DISPATCH_INDIRECT);
	jobs_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_brick_jobs) * 32,
			zeroed(static_cast<int64_t>(cfg_.max_brick_jobs) * 32));
	op_pool_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_region_slots) * ve::kMaxRegionOps * 32,
			zeroed(static_cast<int64_t>(cfg_.max_region_slots) * ve::kMaxRegionOps * 32));
	op_counts_ = rd->storage_buffer_create(static_cast<uint32_t>(cfg_.max_region_slots) * 4,
			zeroed(static_cast<int64_t>(cfg_.max_region_slots) * 4));
	region_slot_counts_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg_.max_region_slots) * 4,
			zeroed(static_cast<int64_t>(cfg_.max_region_slots) * 4));

	// 8 KB per region slot: 4 MB at the shipping 512 slots.
	region_occupancy_ = rd->storage_buffer_create(
			static_cast<uint32_t>(cfg.max_region_slots) * ve::kOccupancyBlockBytes,
			zeroed(static_cast<int64_t>(cfg.max_region_slots) * ve::kOccupancyBlockBytes));

	if (!volumes_.initialize(rd, ve::kMaxVolumes, ve::kIslandDim) ||
			!overrides_.initialize(rd, cfg_.max_override_bricks, cfg_.max_region_slots)) {
		teardown();
		return false;
	}

	// 32 MiB at default settings, INCLUDING both offset tables. Metadata that does not
	// fit is an initialization failure, never an implicit growth.
	if (!stored_normals_.initialize(rd, cfg_.normal_pool_bytes, ve::kMaxVolumes,
			cfg_.max_override_bricks)) {
		UtilityFunctions::printerr("GpuAtlas: stored-normal pool does not fit its budget");
		teardown();
		return false;
	}

	bool ok = sdf_atlas_.is_valid() && mat_atlas_.is_valid() && palette_.is_valid() &&
			brick_flags_.is_valid() && region_map_.is_valid() && region_tables_.is_valid() && free_list_.is_valid() &&
			counters_.is_valid() && frame_.is_valid() && dispatch_args_.is_valid() &&
			jobs_.is_valid() && op_pool_.is_valid() && op_counts_.is_valid() &&
			region_slot_counts_.is_valid() && region_occupancy_.is_valid() && volumes_.is_valid() &&
			overrides_.is_valid() && stored_normals_.is_valid();
	for (int l = 0; l < ve::kMipLevels; l++) ok = ok && mips_[l].is_valid();
	if (!ok) {
		// Most likely cause: the driver refuses STORAGE usage on R8_UNORM / R8G8_UINT
		// (Vulkan's shaderStorageImageExtendedFormats). Fail soft and say so.
		UtilityFunctions::printerr(
				"GpuAtlas: resource creation failed (check storage-image format support)");
		teardown();
		return false;
	}
	return true;
}

void GpuAtlas::teardown() {
	// The pool first: its buffers are referenced by consumer uniform sets (raymarch
	// bindings), and teardown order must live in exactly one place.
	stored_normals_.teardown();
	volumes_.teardown();
	overrides_.teardown();
	if (!rd_) return;
	free_if_valid(rd_, sdf_atlas_);
	free_if_valid(rd_, mat_atlas_);
	for (int l = 0; l < ve::kMipLevels; l++) free_if_valid(rd_, mips_[l]);
	free_if_valid(rd_, palette_);
	free_if_valid(rd_, brick_flags_);
	free_if_valid(rd_, region_map_);
	free_if_valid(rd_, region_tables_);
	free_if_valid(rd_, free_list_);
	free_if_valid(rd_, counters_);
	free_if_valid(rd_, frame_);
	free_if_valid(rd_, dispatch_args_);
	free_if_valid(rd_, jobs_);
	free_if_valid(rd_, op_pool_);
	free_if_valid(rd_, op_counts_);
	free_if_valid(rd_, region_slot_counts_);
	free_if_valid(rd_, region_occupancy_);
	rd_ = nullptr;
}

void GpuAtlas::reset_frame_counters(RenderingDevice *rd) {
	if (!frame_.is_valid()) return;
	// Only the job counter. The overflow word is STICKY: it is read back on the render
	// thread, where nothing stalls for a submit, so a per-frame reset would race the
	// readback and a frame's worth of dropped bricks could go unreported — and a dropped
	// brick that nobody hears about is a hole in the world that never heals. Whoever acts
	// on the bits clears them with clear_overflow().
	rd->buffer_update(frame_, 0, 4, zeroed(4));
}

void GpuAtlas::clear_overflow(RenderingDevice *rd) {
	if (!frame_.is_valid()) return;
	rd->buffer_update(frame_, 4, 4, zeroed(4));
}

int GpuAtlas::read_free_count(RenderingDevice *rd) const {
	if (!counters_.is_valid()) return 0;
	const PackedByteArray b = rd->buffer_get_data(counters_, 0, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : 0;
}

int GpuAtlas::read_job_count(RenderingDevice *rd) const {
	if (!frame_.is_valid()) return 0;
	const PackedByteArray b = rd->buffer_get_data(frame_, 0, 4);
	return b.size() >= 4 ? *reinterpret_cast<const int32_t *>(b.ptr()) : 0;
}

uint32_t GpuAtlas::read_overflow(RenderingDevice *rd) const {
	if (!frame_.is_valid()) return 0;
	const PackedByteArray b = rd->buffer_get_data(frame_, 4, 4);
	return b.size() >= 4 ? *reinterpret_cast<const uint32_t *>(b.ptr()) : 0;
}

void GpuAtlas::read_region_slot_counts(RenderingDevice *rd, std::vector<int> *out) const {
	out->assign(static_cast<size_t>(cfg_.max_region_slots), 0);
	if (!region_slot_counts_.is_valid()) return;
	const PackedByteArray b = rd->buffer_get_data(region_slot_counts_);
	const int64_t n = std::min<int64_t>(b.size() / 4, cfg_.max_region_slots);
	if (n > 0) memcpy(out->data(), b.ptr(), static_cast<size_t>(n) * 4);
}

void GpuAtlas::upload_region_ops(RenderingDevice *rd, int region_slot, const ve::EditOp *ops,
		int count) {
	if (!op_pool_.is_valid() || region_slot < 0 || region_slot >= cfg_.max_region_slots) return;
	count = count < 0 ? 0 : (count > ve::kMaxRegionOps ? ve::kMaxRegionOps : count);
	if (count > 0) {
		PackedByteArray b;
		b.resize(static_cast<int64_t>(count) * 32);
		memcpy(b.ptrw(), ops, static_cast<size_t>(count) * 32);
		rd->buffer_update(op_pool_,
				static_cast<uint32_t>(region_slot) * ve::kMaxRegionOps * 32,
				static_cast<uint32_t>(b.size()), b);
	}
	PackedByteArray c;
	c.resize(4);
	*reinterpret_cast<int32_t *>(c.ptrw()) = count;
	rd->buffer_update(op_counts_, static_cast<uint32_t>(region_slot) * 4, 4, c);
}

void GpuAtlas::set_region_map_entry(RenderingDevice *rd, int region_index, int region_slot) {
	if (!region_map_.is_valid() || region_index < 0 || region_index >= region_map_entries())
		return;
	PackedByteArray b;
	b.resize(4);
	*reinterpret_cast<int32_t *>(b.ptrw()) = region_slot;
	rd->buffer_update(region_map_, static_cast<uint32_t>(region_index) * 4, 4, b);
}

void GpuAtlas::set_override_table(RenderingDevice *rd, int region_slot, int table,
		const std::vector<std::pair<int, int>> &entries) {
	if (!overrides_.is_valid()) return;
	overrides_.set_region_table(rd, region_slot, table);
	for (const auto &entry : entries)
		overrides_.set_table_entry(rd, table, entry.first, entry.second);
}

bool GpuAtlas::replay_overrides(RenderingDevice *rd, const ve::OverrideStore &store,
		const std::map<std::tuple<int, int, int>, int> &tables) {
	if (!rd || !overrides_.is_valid()) return false;
	for (const auto &region_table : tables) {
		const ve::IVec3 region{std::get<0>(region_table.first), std::get<1>(region_table.first),
				std::get<2>(region_table.first)};
		const int table = region_table.second;
		if (table < 0 || table >= OverridePool::kMaxOverrideTables) return false;
		const ve::IVec3 base{region.x * ve::kRegionBricks, region.y * ve::kRegionBricks,
				region.z * ve::kRegionBricks};
		for (int z = 0; z < ve::kRegionBricks; z++)
			for (int y = 0; y < ve::kRegionBricks; y++)
				for (int x = 0; x < ve::kRegionBricks; x++) {
					const ve::IVec3 brick{base.x + x, base.y + y, base.z + z};
					const int slot = store.slot_of(brick);
					if (slot < 0) continue;
					const ve::OverrideBrick *data = store.data(slot);
					if (!data || !overrides_.upload(slot, *data)) return false;
					// The pool this replays into is freshly initialized, so every override
					// offset is -1. Without re-uploading the compact normals here, an atlas
					// teardown/reinit silently drops every consolidated brick back to R8
					// taps -- the artifact this whole feature removes -- and the CPU
					// fallback counter never sees it, because publication succeeded.
					if (data->normal_oct.size() == ve::kBrickSdfCount)
						stored_normals_.upload_override(rd, slot, data->normal_oct.data(),
								ve::kBrickSdfCount);
					else
						stored_normals_.release_override(rd, slot);
					overrides_.set_table_entry(rd, table,
						ve::WorldBounds::brick_index_in_region(brick), slot);
				}
		}
	return true;
}

void GpuAtlas::clear_region_map(RenderingDevice *rd) {
	if (region_map_.is_valid()) {
		const PackedByteArray b = filled_i32(region_map_entries(), -1);
		rd->buffer_update(region_map_, 0, static_cast<uint32_t>(b.size()), b);
	}
	if (brick_flags_.is_valid()) {
		const PackedByteArray b = filled_i32(atlas_slot_count(),
				static_cast<int32_t>(ve::kBrickFlagConservative));
		rd->buffer_update(brick_flags_, 0, static_cast<uint32_t>(b.size()), b);
	}
}
