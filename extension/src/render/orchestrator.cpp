#include "render/orchestrator.h"
#include "gpu_layout/blocks.h"

#include "render/gpu_atlas.h"
#include "render/material_atlas.h"
#include "render/island_atlas.h"
#include "render/island_cull_pass.h"
#include "render/region_pass.h"
#include "render/brick_gen_pass.h"
#include "render/field_context_set.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/deferred_pass.h"
#include "render/inject_pass.h"
#include "render/gbuffer.h"
#include "render/beauty_camera.h"
#include "render/sun_ubo.h"
#include "render/contact_shadow_pass.h"
#include "render/ssgi_pass.h"
#include "render/ssao_pass.h"
#include "render/ssr_pass.h"
#include "render/outline_pass.h"
#include "render/grass_scatter_pass.h"
#include "render/leaf_scatter_pass.h"
#include "render/leaf_raster_pass.h"
#include "render/grass_raster_pass.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/lod_cull_pass.h"
#include "render/hiz_pass.h"
#include "render/world_streamer.h"
#include "lod/lod_system.h"
#include "core/world_store.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>
#include <utility>

using namespace godot;

namespace godot {

RenderOrchestrator::RenderOrchestrator(Collaborators handles) :
		handles_(std::move(handles)), frame_(*this, *handles_.lod, *handles_.store) {
	// The atomics and beauty_ already hold RenderSettings{}'s values, so nothing is mirrored
	// until the first write.
	render_settings_.set_listener(&RenderOrchestrator::on_render_resolved, this);
}

RenderingDevice *RenderOrchestrator::acquire_device() {
	// Guard: main/render-thread use OUTSIDE a frame only. Mid-frame acquisition (e.g.
	// from a compositor callback) would create/fetch a RenderingDevice under the
	// renderer's feet; every call site today runs during ensure_initialized()/reload
	// re-init, before or after the frame's draw lists. Do not call from render work.
	if (*handles_.use_local_device && !local_rd_) {
		local_rd_ = RenderingServer::get_singleton()->create_local_rendering_device();
	} else if (!*handles_.use_local_device && !main_rd_) {
		main_rd_ = RenderingServer::get_singleton()->get_rendering_device();
	}
	return rd();
}

RenderingDevice *RenderOrchestrator::rd() const {
	return *handles_.use_local_device ? local_rd_ : main_rd_;
}

void RenderOrchestrator::release_devices() {
	if (local_rd_) {
		memdelete(local_rd_);
		local_rd_ = nullptr;
	}
	main_rd_ = nullptr;
}

int RenderOrchestrator::drain_island_uploads(RenderingDevice *device) {
	if (!device) return 0;
	IslandHandoff::Batch batch = handoff_.take();
	for (const int slot : batch.normal_releases) {
		if (passes_.atlas) passes_.atlas->stored_normals().release_volume(device, slot);
	}
	for (const IslandHandoff::Upload &u : batch.uploads) {
		// SDF/material and compact normals land ONCE, in the shared authoritative pools,
		// indexed by the volume slot. An island upload additionally refreshes its mip at
		// the atlas slot; a field-volume upload follows the identical volume/normal path
		// without one. A missing/malformed/failed normal payload is fail-soft: the pool
		// publishes -1 and the shader falls back to differentiating the R8 atlas.
		if (passes_.atlas && u.volume_slot >= 0) {
			if (!passes_.atlas->volumes().upload(device, u.volume_slot, u.data))
				UtilityFunctions::printerr("VoxelWorld: field volume upload failed for slot ",
						u.volume_slot);
			passes_.atlas->stored_normals().upload_volume(device, u.volume_slot, u.data);
		} else if (!passes_.atlas && u.to_island_atlas) {
			UtilityFunctions::printerr("VoxelWorld: no GpuAtlas for island upload of slot ",
					u.volume_slot);
		}
		if (u.to_island_atlas && passes_.islands && u.atlas_slot >= 0 &&
				!passes_.islands->upload_mip(device, u.atlas_slot, u.data))
			UtilityFunctions::printerr("VoxelWorld: island mip upload failed for slot ",
					u.atlas_slot);
		if (!u.to_island_atlas) handoff_.note_field_volume_uploaded();
	}
	if (batch.descs_dirty && passes_.islands)
		passes_.islands->upload_descriptors(device, batch.descs.data(), static_cast<int>(batch.descs.size()));
	return static_cast<int>(batch.uploads.size());
}

bool RenderOrchestrator::initialize_downsample(RenderingDevice *rd) {
	teardown_downsample();
	if (!rd) return false;
	downsample_ = gpu::compile_compute(rd, downsample_group_, "RenderOrchestrator", "downsample.comp.glsl");
	downsample_sampler_ = gpu::sampler(rd, downsample_group_, RenderingDevice::SAMPLER_FILTER_LINEAR);
	if (!downsample_.valid() || !downsample_sampler_.is_valid()) {
		teardown_downsample();
		return false;
	}
	return true;
}

void RenderOrchestrator::teardown_downsample() {
	if (RenderingDevice *device = rd()) {
		gpu::RdDevice d{device};
		downsample_group_.release(d);
	} else {
		// No device to free on: its RIDs died with it.
		downsample_group_ = gpu::Group();
	}
	downsample_ = gpu::Program();
	downsample_sampler_ = RID();
	downsample_set_ = gpu::SetCache();
}

bool RenderOrchestrator::has_history() const {
	return has_history_ && history_texture_.is_valid() && passes_.gbuffer &&
			passes_.gbuffer->history() == history_texture_;
}

void RenderOrchestrator::finish_beauty_frame(const float view_proj[16]) {
	if (view_proj) std::memcpy(prev_view_proj_, view_proj, sizeof(prev_view_proj_));
	beauty_frame_++;
}

bool RenderOrchestrator::downsample_history(RenderingDevice *rd, RID src, GBuffer &gb) {
	if (!rd || !downsample_.valid() || !gb.history().is_valid()) return false;
	const Vector2i half = gb.half_size();
	gpu::RdDevice device{rd};
	const RID set = downsample_set_.get(device, downsample_group_, downsample_.shader, 0,
			{gpu::sampled(0, downsample_sampler_, src), gpu::image(1, gb.history())});
	if (!set.is_valid()) return false;
	const ve::DownsamplePush push{{half.x, half.y, 0, 0}};
	if (!gpu::dispatch(rd, downsample_.pipeline, {{set, 0}}, gpu::push_bytes(push), gpu::groups(half.x, 8),
			gpu::groups(half.y, 8)))
		return false;
	has_history_ = true;
	history_texture_ = gb.history();
	return true;
}

RenderOrchestrator::GpuInitResult RenderOrchestrator::ensure_gpu_graph(
		RenderingDevice *device) {
	passes_.atlas = new GpuAtlas();
	GpuAtlasConfig cfg;
	const ve::WorldConfig &config = handles_.store->config();
	cfg.atlas_bricks = {config.atlas_bricks.x, config.atlas_bricks.y, config.atlas_bricks.z};
	cfg.max_region_slots = config.max_region_slots;
	cfg.max_brick_jobs = config.max_brick_jobs;
	cfg.max_override_bricks = config.max_override_bricks;
	cfg.region_window = ve::region_window_centered(0.0f, 0.0f, 0.0f,
			ve::region_window_dim(config.residency_radius_m, ve::ResidencyConfig{}.evict_margin));
	if (normal_pool_bytes_ > 0) cfg.normal_pool_bytes = normal_pool_bytes_; // test initializer
	if (!passes_.atlas->initialize(device, cfg)) { delete passes_.atlas; passes_.atlas = nullptr; return GpuInitResult::kAtlasFailed; }
	passes_.islands = new IslandAtlas();
	if (!passes_.islands->initialize(device)) return GpuInitResult::kFailed;
	passes_.island_cull = new IslandCullPass();
	if (!passes_.island_cull->initialize(device)) return GpuInitResult::kFailed;
	passes_.region = new RegionPass();
	if (!passes_.region->initialize(device, *passes_.atlas)) return GpuInitResult::kFailed;
	passes_.gen = new BrickGenPass();
	if (!passes_.gen->initialize(device, *passes_.atlas)) return GpuInitResult::kFailed;
	passes_.field_context = new FieldContextSet();
	{
		// The set-1 contents come from the stored terrain pipeline
		// (VoxelWorld::load_terrain_pipeline ran before this graph build). An empty
		// pipeline -- load failure -- yields one zeroed vec4 of params and no sampled
		// resources, which is exactly the fallback stub field.glslh declares, so the
		// bind-everywhere invariant holds in both worlds. Fail-soft like the other
		// optional passes: a failed set build leaves the pointer null and the passes
		// skip their set-1 bind.
		if (!passes_.field_context->initialize(device, passes_.gen->shader(),
				handles_.store->terrain_pipeline())) {
			UtilityFunctions::printerr(
					"RenderOrchestrator: field context set creation failed; continuing without set 1");
			delete passes_.field_context;
			passes_.field_context = nullptr;
		}
	}
	passes_.materials = new MaterialAtlas();
	if (!passes_.materials->initialize(device)) return GpuInitResult::kFailed;
	// The four blocks below are the verbatim construction sequence moved into
	// WorldStore; their call positions relative to the GPU setup are load-bearing.
	handles_.store->ensure_edit_log();
	handles_.store->ensure_overrides(passes_.atlas->overrides().capacity());
	if (!passes_.atlas->replay_overrides(device, *handles_.store->overrides(),
			handles_.store->override_tables())) {
		UtilityFunctions::printerr("VoxelWorld: override replay into render pool failed");
		return GpuInitResult::kFailed;
	}
	handles_.store->ensure_residency();
	streamer_ = new WorldStreamer();
	streamer_->initialize(handles_.store->residency(),
			handles_.store->edit_log(), &handles_.store->edit_mutex(),
			handles_.store->pending_edits(), passes_.atlas,
			passes_.region, passes_.gen, handles_.store, handles_.store->overrides(),
			&handles_.store->override_tables(), passes_.field_context);
	passes_.raymarch = new RaymarchPass();
	passes_.raymarch->initialize(device);
	passes_.raymarch->set_materials(*passes_.materials);
	passes_.composite = new CompositePass();
	passes_.composite->initialize(device);
	passes_.deferred = new DeferredPass();
	passes_.deferred->initialize(device);
	passes_.inject = new InjectPass();
	passes_.inject->initialize(device);
	passes_.gbuffer = new GBuffer();
	passes_.beauty_camera = new CameraUbo();
	passes_.contact_shadow = new ContactShadowPass();
	passes_.contact_shadow->initialize(device);
	passes_.sun_ubo = new SunUbo();
	if (!passes_.sun_ubo->ensure(device)) {
		UtilityFunctions::printerr("RenderOrchestrator: sun UBO creation failed");
		delete passes_.sun_ubo;
		passes_.sun_ubo = nullptr;
	} else {
		passes_.sun_ubo->update(device, ve::SunState());
		if (passes_.raymarch) passes_.raymarch->set_sun_ubo(passes_.sun_ubo->buffer());
		if (passes_.deferred) passes_.deferred->set_sun_ubo(passes_.sun_ubo->buffer());
		if (passes_.contact_shadow) passes_.contact_shadow->set_sun_ubo(passes_.sun_ubo->buffer());
	}
	passes_.ssgi = new SsgiPass();
	passes_.ssgi->initialize(device);
	passes_.ssao = new SsaoPass();
	passes_.ssao->initialize(device);
	passes_.ssr = new SsrPass();
	passes_.ssr->initialize(device);
	passes_.outline = new OutlinePass();
	passes_.outline->initialize(device);
	initialize_downsample(device);
	passes_.lod_raster = new LodRasterPass();
	passes_.lod_raster->initialize(device);
	passes_.sun_shadow = new SunShadowPass();
	if (!passes_.sun_shadow->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: sun shadow initialization failed; continuing without "
				"the world shadow map");
		delete passes_.sun_shadow;
		passes_.sun_shadow = nullptr;
	}
	passes_.lod_cull = new LodCullPass();
	if (!passes_.lod_cull->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: LoD cull initialization failed; continuing "
				"without GPU culling (safe fail-soft: draw every candidate page)");
		delete passes_.lod_cull;
		passes_.lod_cull = nullptr;
	}
	passes_.grass_scatter = new GrassScatterPass();
	if (!passes_.grass_scatter->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: grass initialization failed; continuing "
				"without grass (safe fail-soft: the field is simply bare)");
		delete passes_.grass_scatter;
		passes_.grass_scatter = nullptr;
	}
	passes_.grass_raster = new GrassRasterPass();
	passes_.grass_raster->initialize(device);
	passes_.leaf_scatter = new LeafScatterPass();
	if (!passes_.leaf_scatter->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: leaf initialization failed; continuing "
				"without canopies (safe fail-soft: trunks stand bare)");
		delete passes_.leaf_scatter;
		passes_.leaf_scatter = nullptr;
	}
	// Fail-soft like grass_raster: a shader that will not compile leaves initialize() with
	// no shader, draw() returns false, and the frame's timing marker is cancelled -- the
	// scatter still runs, so the chop contract holds and the canopy simply does not draw.
	passes_.leaf_raster = new LeafRasterPass();
	passes_.leaf_raster->initialize(device);
	passes_.hiz = new HizPass();
	if (!passes_.hiz->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: HiZ initialization failed; continuing without "
				"occlusion (safe fail-soft: always visible)");
		delete passes_.hiz;
		passes_.hiz = nullptr;
	}
	return GpuInitResult::kOk;
}

void RenderOrchestrator::teardown_render_passes() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order). Islands sit between
	// passes and the atlas pool: RaymarchPass's uniform set references island buffers too.
	if (passes_.composite) { delete passes_.composite; passes_.composite = nullptr; }
	if (passes_.inject) { delete passes_.inject; passes_.inject = nullptr; }
	if (passes_.deferred) { delete passes_.deferred; passes_.deferred = nullptr; }
	if (passes_.sun_shadow) { delete passes_.sun_shadow; passes_.sun_shadow = nullptr; }
	if (passes_.hiz && passes_.gbuffer) passes_.hiz->release_level0_set();
	teardown_downsample();
	if (passes_.contact_shadow) { delete passes_.contact_shadow; passes_.contact_shadow = nullptr; }
	if (passes_.ssr) { delete passes_.ssr; passes_.ssr = nullptr; }
	if (passes_.outline) { delete passes_.outline; passes_.outline = nullptr; }
	if (passes_.grass_raster) { delete passes_.grass_raster; passes_.grass_raster = nullptr; }
	if (passes_.grass_scatter) { delete passes_.grass_scatter; passes_.grass_scatter = nullptr; }
	if (passes_.leaf_raster) { delete passes_.leaf_raster; passes_.leaf_raster = nullptr; }
	if (passes_.leaf_scatter) { delete passes_.leaf_scatter; passes_.leaf_scatter = nullptr; }
	if (passes_.ssgi) { delete passes_.ssgi; passes_.ssgi = nullptr; }
	if (passes_.ssao) { delete passes_.ssao; passes_.ssao = nullptr; }
	if (passes_.lod_raster) { delete passes_.lod_raster; passes_.lod_raster = nullptr; }
	if (passes_.beauty_camera) { passes_.beauty_camera->teardown(); delete passes_.beauty_camera; passes_.beauty_camera = nullptr; }
	if (passes_.gbuffer) { delete passes_.gbuffer; passes_.gbuffer = nullptr; }
	if (passes_.raymarch) { delete passes_.raymarch; passes_.raymarch = nullptr; }
	if (passes_.sun_ubo) { passes_.sun_ubo->teardown(); delete passes_.sun_ubo; passes_.sun_ubo = nullptr; }
	if (passes_.lod_cull) { delete passes_.lod_cull; passes_.lod_cull = nullptr; }
	if (passes_.hiz) {
		passes_.hiz->teardown();
		last_hiz_readback_was_pending_ = passes_.hiz->readback_was_pending_at_teardown();
		last_hiz_readback_was_drained_ = passes_.hiz->readback_was_drained_at_teardown();
		delete passes_.hiz;
		passes_.hiz = nullptr;
	}
	if (passes_.materials) { delete passes_.materials; passes_.materials = nullptr; }
	if (passes_.field_context) { delete passes_.field_context; passes_.field_context = nullptr; }
	if (passes_.gen) { delete passes_.gen; passes_.gen = nullptr; }
	if (passes_.region) { delete passes_.region; passes_.region = nullptr; }
}

void RenderOrchestrator::teardown_island_graph() {
	if (passes_.island_cull) { delete passes_.island_cull; passes_.island_cull = nullptr; }
	if (passes_.islands) { delete passes_.islands; passes_.islands = nullptr; }
}

void RenderOrchestrator::teardown_atlas_pool() {
	if (passes_.atlas) { delete passes_.atlas; passes_.atlas = nullptr; }
}

void RenderOrchestrator::reset_history_state() {
	has_history_ = false;
	history_texture_ = RID();
	beauty_frame_ = 0;
	std::memset(prev_view_proj_, 0, sizeof(prev_view_proj_));
}

// --- compositor admission/lifetime (moved VERBATIM from VoxelWorld, Task 13) ------
// Same cv waits/signals and latch semantics as the pre-move bodies; nothing here is
// a locking change, only a change of which class's members are touched.

bool RenderOrchestrator::try_begin_render_callback() {
	std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
	if (render_shutting_down_) return false;
	render_callbacks_++;
	return true;
}

void RenderOrchestrator::end_render_callback() {
	bool defer_teardown = false;
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		if (render_callbacks_ <= 0) return;
		if (--render_callbacks_ == 0) {
			defer_teardown = render_teardown_deferred_;
			render_teardown_deferred_ = false;
			render_lifetime_cv_.notify_all();
		}
	}
	// A render-thread caller cannot wait for its own callback guard. Defer destruction until
	// that guard has released the last callback; no resource is touched after this destructor.
	if (defer_teardown) shutdown_render_resources_on_render_thread();
}

void RenderOrchestrator::reopen_admission() {
	std::lock_guard<std::mutex> lifetime(render_lifetime_mutex_);
	render_shutting_down_ = false;
	render_teardown_deferred_ = false;
}

void RenderOrchestrator::close_admission() {
	std::lock_guard<std::mutex> lifetime(render_lifetime_mutex_);
	render_shutting_down_ = true;
}

bool RenderOrchestrator::shutdown_in_progress() {
	std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
	return render_shutting_down_;
}

void RenderOrchestrator::teardown_gpu() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order). Islands sit between
	// passes and the atlas pool: RaymarchPass's uniform set references island buffers too.
	// The deletion sequence lives in the three teardown_*() halves below; the interleaved
	// world-owned statements keep their exact positions in the deallocation order via the
	// Collaborator addresses (Task 13 move -- placement unchanged).
	teardown_trace_.clear();
	teardown_render_passes();
	teardown_trace_.push_back("passes");
	if (streamer_) {
		streamer_->drain_readbacks(rd());
		delete streamer_;
		streamer_ = nullptr;
	}
	teardown_trace_.push_back("streamer");
	handles_.store->clear_residency(); // slot assignments are meaningless pre-atlas
	teardown_trace_.push_back("residency");
	teardown_island_graph();
	teardown_trace_.push_back("island_graph");
	// island_slot_count() can still be on the render thread during teardown; the mark is atomic.
	handoff_.reset_debug_slots();
	teardown_trace_.push_back("island_slots");
	teardown_atlas_pool();
	teardown_trace_.push_back("atlas");
	// The tree holds page indices the pool is about to free, and a stale index would be
	// handed to the next chunk. Pool first, then tree, then the page map.
	handles_.lod->release_gpu();
	teardown_trace_.push_back("lod");
	reset_history_state();
	teardown_trace_.push_back("history");
	initialized_ = false;
	teardown_trace_.push_back("initialized");
}

void RenderOrchestrator::shutdown_render_resources_on_render_thread() {
	teardown_gpu();
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		gpu_teardown_done_ = true;
	}
	gpu_teardown_cv_.notify_all();
}

void RenderOrchestrator::shutdown_render_resources() {
	// Close admission before synchronizing or queueing teardown. The admission lock makes
	// the enabled check and world lookup indivisible from this transition.
	voxel_compositor_callbacks_shutdown_started(this);
	{
		std::unique_lock<std::mutex> lock(render_lifetime_mutex_);
		const bool on_render_thread = RenderingServer::get_singleton()->is_on_render_thread();
		if (on_render_thread && render_callbacks_ > 0) {
			render_teardown_deferred_ = true;
			return;
		}
		if (!on_render_thread) {
			render_lifetime_cv_.wait(lock, [this] { return render_callbacks_ == 0; });
		}
	}
	if (!initialized_ || !rd()) return;
	if (RenderingServer::get_singleton()->is_on_render_thread() || *handles_.use_local_device ||
			!has_main_device()) {
		teardown_gpu();
		return;
	}
	// Drain the RenderingServer queue first; unlike RenderingDevice::submit/sync this is the
	// supported global-device synchronization boundary. The actual RD teardown is queued on
	// the render thread, where HizPass can drain its pending async Callable safely.
	RenderingServer::get_singleton()->force_sync();
	{
		std::lock_guard<std::mutex> lock(render_lifetime_mutex_);
		gpu_teardown_done_ = false;
	}
	RenderingServer::get_singleton()->call_on_render_thread(
			Callable(handles_.callback_owner, kShutdownRenderResourcesOnRenderThread));
	std::unique_lock<std::mutex> lock(render_lifetime_mutex_);
	gpu_teardown_cv_.wait(lock, [this] { return gpu_teardown_done_; });
}

bool RenderOrchestrator::preflight_shaders(RenderingDevice *rd, String *out_error) {
	if (!rd) {
		if (out_error) *out_error = "shader reload pre-flight: no RenderingDevice";
		return false;
	}
	Ref<DirAccess> dir = DirAccess::open("res://shaders");
	if (dir.is_null()) {
		if (out_error) *out_error = "shader reload pre-flight: cannot open res://shaders";
		return false;
	}
	dir->list_dir_begin();
	String file = dir->get_next();
	bool ok = true;
	while (!file.is_empty()) {
		if (!dir->current_is_dir() && file.ends_with(".glsl")) {
			const String res = "res://shaders/" + file;
			RenderingDevice::ShaderStage stage = RenderingDevice::SHADER_STAGE_COMPUTE;
			if (file.ends_with(".vert.glsl")) stage = RenderingDevice::SHADER_STAGE_VERTEX;
			else if (file.ends_with(".frag.glsl")) stage = RenderingDevice::SHADER_STAGE_FRAGMENT;
			if (!gpu::compile_check(rd, res, stage, out_error)) {
				ok = false;
				break;
			}
		}
		file = dir->get_next();
	}
	dir->list_dir_end();
	return ok;
}

namespace {

// set/get_effect_value address the knobs that are a magnitude; switches go through
// set/get_effect_enabled.
bool is_magnitude(ve::SettingKind kind) {
	return kind == ve::SettingKind::kInt || kind == ve::SettingKind::kFloat;
}

} // namespace

// --- shader hot-reload machinery + beauty settings (moved VERBATIM from VoxelWorld,
// Task 14) ---------------------------------------------------------------
// Same latch/mutex semantics as the pre-move bodies: request_shader_reload() only sets
// the atomic latch (safe from _input); pump_shader_reload() runs where the render
// callback runs and takes each mutex exactly where VoxelWorld took it. Nothing here is a
// locking change, only a change of which class's members are touched.

void RenderOrchestrator::request_shader_reload() {
	// A latch, not the work: shaders are compiled and pipelines created on the device that
	// owns them, and for the shipping world that device belongs to the render thread.
	reload_requested_.store(true, std::memory_order_release);
}

void RenderOrchestrator::pump_shader_reload() {
	if (!reload_requested_.exchange(false, std::memory_order_acq_rel)) return;
	if (shutdown_in_progress()) return; // same mutex-guarded latch check as before the Task 13 move
	{
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_count_++;
	}
	if (!initialized_) {
		handles_.ensure_initialized();
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_last_ok_ = initialized_;
		reload_last_error_ = initialized_ ? String()
											   : String("shader reload re-init failed");
		return;
	}
	String error;
	if (!preflight_shaders(rd(), &error)) {
		// Fail-soft (spec §8): a shader that will not compile must not take down the
		// pipelines that are already running. Keep the old GPU objects untouched.
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_last_ok_ = false;
		reload_last_error_ = error;
		UtilityFunctions::printerr("VoxelWorld: shader reload pre-flight failed; keeping old pipelines: ",
				error);
		return;
	}
	// teardown_gpu() frees every GPU object and leaves the CPU cores -- edit log, residency,
	// override store, LoD tree -- untouched, so ensure_initialized() re-streams the same
	// world. This is the whole hot reload.
	teardown_gpu();
	handles_.ensure_initialized();
	{
		std::lock_guard<std::mutex> lock(reload_mutex_);
		reload_last_ok_ = initialized_;
		reload_last_error_ = initialized_ ? String()
											   : String("shader reload re-init failed");
	}
}

void RenderOrchestrator::reload_snapshot(int *out_count, bool *out_last_ok,
		String *out_last_error) const {
	std::lock_guard<std::mutex> lock(reload_mutex_);
	*out_count = reload_count_;
	*out_last_ok = reload_last_ok_;
	*out_last_error = reload_last_error_;
}

void RenderOrchestrator::on_render_resolved(const ve::RenderSettings &s, void *ctx) {
	auto *self = static_cast<RenderOrchestrator *>(ctx);
	self->islands_enabled_.store(s.islands, std::memory_order_relaxed);
	self->near_field_enabled_.store(s.near_field, std::memory_order_relaxed);
	self->near_field_scale_.store(s.near_field_scale, std::memory_order_relaxed);
	// Rebase: the tier is the base, per-knob overrides layer on top and survive (S6).
	if (self->quality_tier_.exchange(s.quality_tier, std::memory_order_relaxed) != s.quality_tier)
		self->beauty_.rebase(ve::settings_for_tier(static_cast<ve::QualityTier>(s.quality_tier)));
}

ve::SettingsGroup *RenderOrchestrator::settings_group(const char *name) {
	if (!name) return nullptr;
	if (std::strcmp(name, "render") == 0) return &render_settings_;
	if (std::strcmp(name, "beauty") == 0) return &beauty_;
	if (std::strcmp(name, "grass") == 0) return &grass_settings_;
	return nullptr;
}

void RenderOrchestrator::set_quality_tier(int v) {
	render_settings_.set_value("quality_tier", static_cast<float>(v));
}

int RenderOrchestrator::quality_tier() const {
	return quality_tier_.load(std::memory_order_relaxed);
}

void RenderOrchestrator::set_effect_enabled(const String &name, bool on) {
	const CharString n = name.utf8();
	const ve::SettingValue v = ve::SettingValue::of_bool(on);
	// Render switches (islands, near_field), then beauty switches; fail-soft for anything else.
	if (!render_settings_.set(n.get_data(), v)) beauty_.set(n.get_data(), v);
}

bool RenderOrchestrator::get_effect_enabled(const String &name) const {
	const CharString n = name.utf8();
	ve::SettingValue v;
	if (!render_settings_.get(n.get_data(), &v) && !beauty_.get(n.get_data(), &v)) return false;
	return v.kind == ve::SettingKind::kBool && v.v[0] != 0.0f;
}

void RenderOrchestrator::set_effect_value(const String &name, float value) {
	const CharString n = name.utf8();
	ve::SettingValue current;
	if (!beauty_.get(n.get_data(), &current) || !is_magnitude(current.kind)) return; // fail-soft
	beauty_.set_value(n.get_data(), value);
}

float RenderOrchestrator::get_effect_value(const String &name) const {
	const CharString n = name.utf8();
	ve::SettingValue v;
	if (!beauty_.get(n.get_data(), &v) || !is_magnitude(v.kind)) return 0.0f;
	return v.v[0];
}

ve::BeautySettings RenderOrchestrator::beauty_settings() const {
	return beauty_.get();
}

void RenderOrchestrator::beauty_snapshot(ve::BeautySettings *out_settings, int *out_tier) const {
	*out_settings = beauty_.get();
	*out_tier = quality_tier();
}

void RenderOrchestrator::set_sun_state(const ve::SunState &sun) {
	std::lock_guard<std::mutex> lock(sun_mutex_);
	sun_state_ = sun;
}

ve::SunState RenderOrchestrator::sun_state() const {
	std::lock_guard<std::mutex> lock(sun_mutex_);
	return sun_state_;
}

void RenderOrchestrator::set_near_field_scale(float v) {
	render_settings_.set_value("near_field_scale", v); // the row clamps to [0.1, 1]
}

FrameSettings RenderOrchestrator::frame_settings() const {
	FrameSettings s;
	s.sun = sun_state();
	s.near_field_scale = near_field_scale();
	s.near_field_enabled = near_field_enabled();
	s.sun_cascade_min_level = sun_cascade_min_level_;
	return s;
}

} // namespace godot
