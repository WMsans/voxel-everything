#include "render/orchestrator.h"

#include "render/gpu_atlas.h"
#include "render/material_atlas.h"
#include "render/island_atlas.h"
#include "render/island_cull_pass.h"
#include "render/region_pass.h"
#include "render/brick_gen_pass.h"
#include "render/raymarch_pass.h"
#include "render/composite_pass.h"
#include "render/deferred_pass.h"
#include "render/inject_pass.h"
#include "render/gbuffer.h"
#include "render/beauty_camera.h"
#include "render/contact_shadow_pass.h"
#include "render/ssgi_pass.h"
#include "render/ssr_pass.h"
#include "render/outline_pass.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/lod_cull_pass.h"
#include "render/hiz_pass.h"
#include "render/world_streamer.h"
#include "render/shader_loader.h"
#include "render/lod_pool.h"
#include "lod/lod_tree.h"
#include "core/world_store.h"
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/rd_sampler_state.hpp>
#include <godot_cpp/classes/rd_shader_source.hpp>
#include <godot_cpp/classes/rd_shader_spirv.hpp>
#include <godot_cpp/classes/rd_uniform.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cstring>

using namespace godot;

namespace godot {

RenderOrchestrator::RenderOrchestrator(Collaborators handles) : handles_(handles) {}

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

ve::WorldBounds RenderOrchestrator::world_bounds() const {
	const ve::WorldConfig &c = handles_.store->config();
	ve::WorldBounds b;
	b.origin_bricks = {c.world_origin_bricks.x, c.world_origin_bricks.y, c.world_origin_bricks.z};
	b.size_regions = {c.world_size_regions.x, c.world_size_regions.y, c.world_size_regions.z};
	return b;
}

bool RenderOrchestrator::initialize_downsample(RenderingDevice *rd) {
	teardown_downsample();
	if (!rd) return false;
	const String path = ProjectSettings::get_singleton()->globalize_path(
			"res://shaders/downsample.comp.glsl");
	const String inc = ProjectSettings::get_singleton()->globalize_path("res://shaders");
	std::string err;
	const std::string code = ve::strip_shader_annotations(
			ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
	if (code.empty()) return false;
	Ref<RDShaderSource> source;
	source.instantiate();
	source->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
	source->set_stage_source(RenderingDevice::SHADER_STAGE_COMPUTE, String(code.c_str()));
	Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(source);
	if (!spirv->get_stage_compile_error(RenderingDevice::SHADER_STAGE_COMPUTE).is_empty())
		return false;
	downsample_shader_ = rd->shader_create_from_spirv(spirv);
	downsample_pipeline_ = rd->compute_pipeline_create(downsample_shader_);
	Ref<RDSamplerState> sampler;
	sampler.instantiate();
	sampler->set_min_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	sampler->set_mag_filter(RenderingDevice::SAMPLER_FILTER_LINEAR);
	downsample_sampler_ = rd->sampler_create(sampler);
	if (!downsample_shader_.is_valid() || !downsample_pipeline_.is_valid() ||
			!downsample_sampler_.is_valid()) {
		teardown_downsample();
		return false;
	}
	return true;
}

void RenderOrchestrator::teardown_downsample() {
	RenderingDevice *device = rd();
	if (device) {
		for (RID *r : {&downsample_uset_, &downsample_pipeline_, &downsample_shader_,
				&downsample_sampler_}) {
			if (r->is_valid()) device->free_rid(*r);
			*r = RID();
		}
	}
	downsample_src_ = downsample_dst_ = RID();
}

bool RenderOrchestrator::ensure_downsample_set(RenderingDevice *rd, RID src, RID dst) {
	if (downsample_uset_.is_valid() && downsample_src_ == src && downsample_dst_ == dst)
		return true;
	if (downsample_uset_.is_valid()) rd->free_rid(downsample_uset_);
	Ref<RDUniform> u0, u1;
	u0.instantiate(); u1.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0); u0->add_id(downsample_sampler_); u0->add_id(src);
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u1->set_binding(1); u1->add_id(dst);
	downsample_uset_ = rd->uniform_set_create(Array::make(u0, u1), downsample_shader_, 0);
	if (!downsample_uset_.is_valid()) return false;
	downsample_src_ = src;
	downsample_dst_ = dst;
	return true;
}

void RenderOrchestrator::finish_beauty_frame(const float view_proj[16]) {
	if (view_proj) std::memcpy(prev_view_proj_, view_proj, sizeof(prev_view_proj_));
	beauty_frame_++;
}

bool RenderOrchestrator::downsample_history(RenderingDevice *rd, RID src, GBuffer &gb) {
	if (!rd || !downsample_pipeline_.is_valid() || !gb.history().is_valid()) return false;
	const Vector2i half = gb.half_size();
	if (!ensure_downsample_set(rd, src, gb.history())) return false;
	PackedByteArray pc;
	pc.resize(16);
	int32_t *dims = reinterpret_cast<int32_t *>(pc.ptrw());
	dims[0] = half.x; dims[1] = half.y; dims[2] = dims[3] = 0;
	const int64_t list = rd->compute_list_begin();
	if (list < 0) return false;
	rd->compute_list_bind_compute_pipeline(list, downsample_pipeline_);
	rd->compute_list_bind_uniform_set(list, downsample_uset_, 0);
	rd->compute_list_set_push_constant(list, pc, pc.size());
	rd->compute_list_dispatch(list, (half.x + 7) / 8, (half.y + 7) / 8, 1);
	rd->compute_list_end();
	has_history_ = true;
	return true;
}

RenderOrchestrator::GpuInitResult RenderOrchestrator::ensure_gpu_graph(
		RenderingDevice *device) {
	atlas_ = new GpuAtlas();
	GpuAtlasConfig cfg;
	const ve::WorldConfig &config = handles_.store->config();
	cfg.atlas_bricks = {config.atlas_bricks.x, config.atlas_bricks.y, config.atlas_bricks.z};
	cfg.max_region_slots = config.max_region_slots;
	cfg.max_brick_jobs = config.max_brick_jobs;
	cfg.max_override_bricks = config.max_override_bricks;
	cfg.bounds = world_bounds();
	if (*handles_.normal_pool_bytes > 0) cfg.normal_pool_bytes = *handles_.normal_pool_bytes; // test initializer
	if (!atlas_->initialize(device, cfg)) { delete atlas_; atlas_ = nullptr; return GpuInitResult::kAtlasFailed; }
	islands_ = new IslandAtlas();
	if (!islands_->initialize(device)) return GpuInitResult::kFailed;
	island_cull_ = new IslandCullPass();
	if (!island_cull_->initialize(device)) return GpuInitResult::kFailed;
	region_pass_ = new RegionPass();
	if (!region_pass_->initialize(device, *atlas_)) return GpuInitResult::kFailed;
	gen_pass_ = new BrickGenPass();
	if (!gen_pass_->initialize(device, *atlas_)) return GpuInitResult::kFailed;
	materials_ = new MaterialAtlas();
	if (!materials_->initialize(device)) return GpuInitResult::kFailed;
	// The four blocks below are the verbatim construction sequence moved into
	// WorldStore; their call positions relative to the GPU setup are load-bearing.
	handles_.store->ensure_edit_log(world_bounds());
	handles_.store->ensure_overrides(atlas_->overrides().capacity());
	if (!atlas_->replay_overrides(device, *handles_.store->overrides(),
			handles_.store->override_tables())) {
		UtilityFunctions::printerr("VoxelWorld: override replay into render pool failed");
		return GpuInitResult::kFailed;
	}
	handles_.store->ensure_residency(world_bounds());
	*handles_.streamer = new WorldStreamer();
	(*handles_.streamer)->initialize(handles_.store->residency(),
			handles_.store->edit_log(), &handles_.store->edit_mutex(),
			handles_.store->pending_edits(), atlas_,
			region_pass_, gen_pass_, handles_.store, handles_.store->overrides(),
			&handles_.store->override_tables());
	raymarch_pass_ = new RaymarchPass();
	raymarch_pass_->initialize(device);
	raymarch_pass_->set_materials(*materials_);
	composite_pass_ = new CompositePass();
	composite_pass_->initialize(device);
	deferred_pass_ = new DeferredPass();
	deferred_pass_->initialize(device);
	inject_pass_ = new InjectPass();
	inject_pass_->initialize(device);
	gbuffer_ = new GBuffer();
	beauty_camera_ = new CameraUbo();
	contact_shadow_pass_ = new ContactShadowPass();
	contact_shadow_pass_->initialize(device);
	ssgi_pass_ = new SsgiPass();
	ssgi_pass_->initialize(device);
	ssr_pass_ = new SsrPass();
	ssr_pass_->initialize(device);
	outline_pass_ = new OutlinePass();
	outline_pass_->initialize(device);
	initialize_downsample(device);
	lod_raster_pass_ = new LodRasterPass();
	lod_raster_pass_->initialize(device);
	sun_shadow_pass_ = new SunShadowPass();
	if (!sun_shadow_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: sun shadow initialization failed; continuing without "
				"the world shadow map");
		delete sun_shadow_pass_;
		sun_shadow_pass_ = nullptr;
	}
	lod_cull_pass_ = new LodCullPass();
	if (!lod_cull_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: LoD cull initialization failed; continuing "
				"without GPU culling (safe fail-soft: draw every candidate page)");
		delete lod_cull_pass_;
		lod_cull_pass_ = nullptr;
	}
	hiz_pass_ = new HizPass();
	if (!hiz_pass_->initialize(device)) {
		UtilityFunctions::printerr("VoxelWorld: HiZ initialization failed; continuing without "
				"occlusion (safe fail-soft: always visible)");
		delete hiz_pass_;
		hiz_pass_ = nullptr;
	}
	return GpuInitResult::kOk;
}

void RenderOrchestrator::teardown_render_passes() {
	// Passes before the atlas: their uniform sets reference atlas RIDs, and freeing a
	// texture cascades to referencing sets (M1's documented order). Islands sit between
	// passes and the atlas pool: RaymarchPass's uniform set references island buffers too.
	if (composite_pass_) { delete composite_pass_; composite_pass_ = nullptr; }
	if (inject_pass_) { delete inject_pass_; inject_pass_ = nullptr; }
	if (deferred_pass_) { delete deferred_pass_; deferred_pass_ = nullptr; }
	if (sun_shadow_pass_) { delete sun_shadow_pass_; sun_shadow_pass_ = nullptr; }
	if (hiz_pass_ && gbuffer_) hiz_pass_->release_level0_set();
	teardown_downsample();
	if (contact_shadow_pass_) { delete contact_shadow_pass_; contact_shadow_pass_ = nullptr; }
	if (ssr_pass_) { delete ssr_pass_; ssr_pass_ = nullptr; }
	if (outline_pass_) { delete outline_pass_; outline_pass_ = nullptr; }
	if (ssgi_pass_) { delete ssgi_pass_; ssgi_pass_ = nullptr; }
	if (beauty_camera_) { beauty_camera_->teardown(); delete beauty_camera_; beauty_camera_ = nullptr; }
	if (gbuffer_) { delete gbuffer_; gbuffer_ = nullptr; }
	if (raymarch_pass_) { delete raymarch_pass_; raymarch_pass_ = nullptr; }
	if (lod_raster_pass_) { delete lod_raster_pass_; lod_raster_pass_ = nullptr; }
	if (lod_cull_pass_) { delete lod_cull_pass_; lod_cull_pass_ = nullptr; }
	if (hiz_pass_) {
		hiz_pass_->teardown();
		*handles_.last_hiz_readback_was_pending = hiz_pass_->readback_was_pending_at_teardown();
		*handles_.last_hiz_readback_was_drained = hiz_pass_->readback_was_drained_at_teardown();
		delete hiz_pass_;
		hiz_pass_ = nullptr;
	}
	if (materials_) { delete materials_; materials_ = nullptr; }
	if (gen_pass_) { delete gen_pass_; gen_pass_ = nullptr; }
	if (region_pass_) { delete region_pass_; region_pass_ = nullptr; }
}

void RenderOrchestrator::teardown_island_graph() {
	if (island_cull_) { delete island_cull_; island_cull_ = nullptr; }
	if (islands_) { delete islands_; islands_ = nullptr; }
}

void RenderOrchestrator::teardown_atlas_pool() {
	if (atlas_) { delete atlas_; atlas_ = nullptr; }
}

void RenderOrchestrator::reset_history_state() {
	has_history_ = false;
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
	teardown_render_passes();
	if (*handles_.streamer) {
		(*handles_.streamer)->drain_readbacks(rd());
		delete *handles_.streamer;
		*handles_.streamer = nullptr;
	}
	handles_.store->clear_residency(); // slot assignments are meaningless pre-atlas
	teardown_island_graph();
	{
		// island_slot_count() can still be on the render thread during teardown; keep the
		// high-water mark's write under the same mutex.
		std::lock_guard<std::mutex> lock(*handles_.island_mutex);
		*handles_.island_slots = 0;
	}
	teardown_atlas_pool();
	// The tree holds page indices the pool is about to free, and a stale index would be
	// handed to the next chunk. Pool first, then tree, then the page map.
	if (*handles_.lod_pool) (*handles_.lod_pool)->teardown();
	if (*handles_.lod_tree) (*handles_.lod_tree)->clear();
	handles_.lod_pages_of->clear();
	handles_.lod_page_quads->clear();
	handles_.lod_overflow_logged->clear();
	reset_history_state();
	*handles_.initialized = false;
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
	if (!*handles_.initialized || !rd()) return;
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
			Callable(handles_.callback_owner, "_shutdown_render_resources_on_render_thread"));
	std::unique_lock<std::mutex> lock(render_lifetime_mutex_);
	gpu_teardown_cv_.wait(lock, [this] { return gpu_teardown_done_; });
}

bool RenderOrchestrator::preflight_shaders(RenderingDevice *rd, String *out_error) {
	if (!rd) {
		if (out_error) *out_error = "shader reload pre-flight: no RenderingDevice";
		return false;
	}
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String inc = ps->globalize_path("res://shaders");
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
			const String path = ps->globalize_path(res);
			std::string err;
			const std::string code = ve::strip_shader_annotations(
					ve::load_shader_source(path.utf8().get_data(), inc.utf8().get_data(), &err));
			if (code.empty()) {
				if (out_error) *out_error = res + String(": ") + String(err.c_str());
				ok = false;
				break;
			}
			Ref<RDShaderSource> src;
			src.instantiate();
			src->set_language(RenderingDevice::SHADER_LANGUAGE_GLSL);
			RenderingDevice::ShaderStage stage = RenderingDevice::SHADER_STAGE_COMPUTE;
			if (file.ends_with(".vert.glsl")) stage = RenderingDevice::SHADER_STAGE_VERTEX;
			else if (file.ends_with(".frag.glsl")) stage = RenderingDevice::SHADER_STAGE_FRAGMENT;
			src->set_stage_source(stage, String(code.c_str()));
			Ref<RDShaderSPIRV> spirv = rd->shader_compile_spirv_from_source(src);
			const String compile_err = spirv->get_stage_compile_error(stage);
			if (!compile_err.is_empty()) {
				if (out_error) *out_error = res + String(": ") + compile_err;
				ok = false;
				break;
			}
		}
		file = dir->get_next();
	}
	dir->list_dir_end();
	return ok;
}

} // namespace godot
