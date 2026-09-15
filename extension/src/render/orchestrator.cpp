#include "render/orchestrator.h"

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
#include "render/grass_raster_pass.h"
#include "render/lod_raster_pass.h"
#include "render/sun_shadow_pass.h"
#include "render/lod_cull_pass.h"
#include "render/hiz_pass.h"
#include "render/world_streamer.h"
#include "render/shader_loader.h"
#include "lod/lod_system.h"
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
#include <utility>

using namespace godot;

namespace godot {

RenderOrchestrator::RenderOrchestrator(Collaborators handles) :
		handles_(std::move(handles)), frame_(*this, *handles_.lod, *handles_.store) {}

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
		if (device->uniform_set_is_valid(downsample_uset_)) device->free_rid(downsample_uset_);
		downsample_uset_ = RID();
		for (RID *r : {&downsample_pipeline_, &downsample_shader_,
				&downsample_sampler_}) {
			if (r->is_valid()) device->free_rid(*r);
			*r = RID();
		}
	}
	downsample_src_ = downsample_dst_ = RID();
}

bool RenderOrchestrator::ensure_downsample_set(RenderingDevice *rd, RID src, RID dst) {
	if (rd->uniform_set_is_valid(downsample_uset_) && downsample_src_ == src && downsample_dst_ == dst)
		return true;
	if (rd->uniform_set_is_valid(downsample_uset_)) rd->free_rid(downsample_uset_);
	Ref<RDUniform> u0, u1;
	u0.instantiate(); u1.instantiate();
	u0->set_uniform_type(RenderingDevice::UNIFORM_TYPE_SAMPLER_WITH_TEXTURE);
	u0->set_binding(0); u0->add_id(downsample_sampler_); u0->add_id(src);
	u1->set_uniform_type(RenderingDevice::UNIFORM_TYPE_IMAGE);
	u1->set_binding(1); u1->add_id(dst);
	downsample_uset_ = rd->uniform_set_create(Array::make(u0, u1), downsample_shader_, 0);
	if (!rd->uniform_set_is_valid(downsample_uset_)) return false;
	downsample_src_ = src;
	downsample_dst_ = dst;
	return true;
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

namespace {

// One table, so the setter, the getter and the debug dictionary cannot disagree about what
// an effect is called. (Moved verbatim from voxel_world.cpp, Task 14.)
bool *beauty_field(ve::BeautySettings &s, const String &name) {
	if (name == "ssgi") return &s.ssgi;
	if (name == "ssr") return &s.ssr;
	if (name == "contact_shadows") return &s.contact_shadows;
	if (name == "outlines") return &s.outlines;
	if (name == "sun_shadow_map") return &s.sun_shadow_map;
	if (name == "glossy_sdf_rays") return &s.glossy_sdf_rays;
	if (name == "raymarched_sun_shadow") return &s.raymarched_sun_shadow;
	if (name == "ssao") return &s.ssao;
	if (name == "cost_view") return &s.cost_view;
	return nullptr;
}

// The same table, for the knobs that are a magnitude rather than a switch. Kept beside
// beauty_field for the same reason it exists: one place decides what a knob is called.
float *beauty_value_field(ve::BeautySettings &s, const String &name) {
	if (name == "ssgi_radius") return &s.ssgi_radius;
	if (name == "ssgi_temporal") return &s.ssgi_temporal;
	if (name == "ssgi_strength") return &s.ssgi_strength;
	if (name == "emissive_gi_radius") return &s.emissive_gi_radius;
	if (name == "emissive_gi_strength") return &s.emissive_gi_strength;
	if (name == "outline_depth_threshold") return &s.outline_depth_threshold;
	if (name == "outline_normal_threshold") return &s.outline_normal_threshold;
	return nullptr;
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

void RenderOrchestrator::set_quality_tier(int v) {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	quality_tier_ = v < 0 ? 0 : (v > 3 ? 3 : v);
	beauty_ = ve::settings_for_tier(static_cast<ve::QualityTier>(quality_tier_));
}

int RenderOrchestrator::quality_tier() const {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	return quality_tier_;
}

void RenderOrchestrator::set_effect_enabled(const String &name, bool on) {
	if (name == "islands") {
		islands_enabled_.store(on, std::memory_order_relaxed);
		return;
	}
	if (name == "near_field") {
		near_field_enabled_.store(on, std::memory_order_relaxed);
		return;
	}
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	bool *f = beauty_field(beauty_, name);
	if (!f) return; // fail-soft: an unknown name in a debug menu is not a crash
	*f = on;
	ve::clamp_settings(&beauty_);
}

bool RenderOrchestrator::get_effect_enabled(const String &name) const {
	if (name == "islands") return islands_enabled_.load(std::memory_order_relaxed);
	if (name == "near_field") return near_field_enabled_.load(std::memory_order_relaxed);
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	ve::BeautySettings copy = beauty_;
	const bool *f = beauty_field(copy, name);
	return f ? *f : false;
}

void RenderOrchestrator::set_effect_value(const String &name, float value) {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	float *f = beauty_value_field(beauty_, name);
	if (!f) return; // fail-soft, exactly as set_effect_enabled treats an unknown name
	*f = value;
	ve::clamp_settings(&beauty_);
}

float RenderOrchestrator::get_effect_value(const String &name) const {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	ve::BeautySettings copy = beauty_;
	const float *f = beauty_value_field(copy, name);
	return f ? *f : 0.0f;
}

ve::BeautySettings RenderOrchestrator::beauty_settings() const {
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	return beauty_;
}

void RenderOrchestrator::beauty_snapshot(ve::BeautySettings *out_settings, int *out_tier) const {
	// One hold, matching the pre-move debug_beauty_settings body's shape.
	std::lock_guard<std::mutex> lock(beauty_mutex_);
	*out_settings = beauty_;
	*out_tier = quality_tier_;
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
	near_field_scale_.store(v < 0.1f ? 0.1f : (v > 1.0f ? 1.0f : v), std::memory_order_relaxed);
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
