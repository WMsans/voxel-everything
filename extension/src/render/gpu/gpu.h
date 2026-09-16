#pragma once
// The pass helper (docs/superpowers/specs/2026-09-15-pass-anatomy-generated-layouts-design.md
// §3.1). Lifetime rules live in the godot-free gpu_core.h; this file binds them to
// RenderingDevice and holds the steps every pass used to copy: compile, samplers, textures,
// sized targets, framebuffers, raster pipelines, push bytes and dispatch.
#include "render/gpu/gpu_core.h"
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <chrono>
#include <cstring>
#include <initializer_list>
#include <type_traits>
#include <vector>

namespace godot::gpu {

using ve::gpu::image;
using ve::gpu::Kind;
using ve::gpu::sampled;
using ve::gpu::storage;
using ve::gpu::ubo;

// ve::gpu's Device concept over a RenderingDevice. A pointer, not an owner: build one on the
// stack where a call needs it.
struct RdDevice {
	using Id = RID;
	RenderingDevice *rd = nullptr;
	bool alive(Kind kind, const RID &id);
	void free(const RID &id);
	RID create_uniform_set(const RID &shader, uint32_t set,
			const std::vector<ve::gpu::Uniform<RID>> &uniforms);
};

using Uniform = ve::gpu::Uniform<RID>;
using Group = ve::gpu::ResourceGroup<RdDevice>;
using SetCache = ve::gpu::UniformSetCache<RdDevice>;

RID uniform_set(RenderingDevice *rd, Group &group, const RID &shader, uint32_t set,
		const std::vector<Uniform> &uniforms);

// ---- compile -------------------------------------------------------------------------------

struct Program {
	RID shader;
	RID pipeline; // compute only: a raster pipeline depends on a framebuffer format
	bool valid() const { return shader.is_valid() && pipeline.is_valid(); }
};

// res://shaders/<file>, expanded, annotation-stripped, `defines` inserted after #version,
// compiled; shader and pipeline registered in `group`. Prints "<label>: <file>: <why>" and
// returns an invalid Program on failure.
Program compile_compute(RenderingDevice *rd, Group &group, const char *label, const char *file,
		const char *defines = "");

// Vertex + fragment, `defines` in both stages. Returns the shader (registered), or RID().
RID compile_raster(RenderingDevice *rd, Group &group, const char *label, const char *vertex_file,
		const char *fragment_file, const char *defines = "");

// Compiles one stage of `res_path` and creates nothing: the shader-reload pre-flight. On
// failure sets *out_error to "<res_path>: <why>".
bool compile_check(RenderingDevice *rd, const String &res_path, RenderingDevice::ShaderStage stage,
		String *out_error);

// ---- resources -----------------------------------------------------------------------------

RID sampler(RenderingDevice *rd, Group &group, RenderingDevice::SamplerFilter filter,
		bool clamp_to_edge = false);

// A 2D texture that never resizes, registered in `group`.
RID texture(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format, Vector2i size,
		uint32_t usage, const TypedArray<PackedByteArray> &data = {});

// A 2D texture recreated whenever the requested size changes. The old one is freed through the
// group, so the uniform sets that bound it die with it and their caches rebuild.
class Target {
public:
	// `clear`, when given, is applied once to each newly created texture.
	bool ensure(RenderingDevice *rd, Group &group, RenderingDevice::DataFormat format, Vector2i size,
			uint32_t usage, const Color *clear = nullptr);
	RID rid() const { return rid_; }
	Vector2i size() const { return size_; }

private:
	RID rid_;
	Vector2i size_{0, 0};
};

// A framebuffer over a list of attachments, reused while the attachment RIDs match and the
// device still has it (a framebuffer dies with any attachment).
class FramebufferCache {
public:
	RID get(RenderingDevice *rd, Group &group, const std::vector<RID> &attachments);
	void release(RenderingDevice *rd, Group &group);
	RID rid() const { return rid_; }
	int64_t format() const { return format_; }

private:
	RID rid_;
	std::vector<RID> attachments_;
	int64_t format_ = 0;
};

// The raster state this engine's passes vary. Defaults are RenderingDevice's own for cull and
// front face, and this engine's reverse-Z depth test (near = 1, far = 0).
struct RasterState {
	RenderingDevice::PolygonCullMode cull = RenderingDevice::POLYGON_CULL_DISABLED;
	RenderingDevice::PolygonFrontFace front = RenderingDevice::POLYGON_FRONT_FACE_CLOCKWISE;
	bool depth_test = true;
	bool depth_write = true;
	RenderingDevice::CompareOperator compare = RenderingDevice::COMPARE_OP_GREATER_OR_EQUAL;
	int color_attachments = 0; // each opaque (blend disabled)
	bool logic_or = false;     // colour logic op OR (the seam-marker probe)
};

// Pull-only triangles: no vertex format.
RID raster_pipeline(RenderingDevice *rd, Group &group, const RID &shader, int64_t fb_format,
		const RasterState &state);

// ---- dispatch ------------------------------------------------------------------------------

template <class T>
PackedByteArray push_bytes(const T &block) {
	static_assert(std::is_trivially_copyable_v<T>, "a push block is copied as bytes");
	PackedByteArray bytes;
	bytes.resize(sizeof(T));
	std::memcpy(bytes.ptrw(), &block, sizeof(T));
	return bytes;
}

struct SetSlot {
	RID set;
	uint32_t index;
};

// One compute list: bind, push (skipped when empty), dispatch, end. False when the list could
// not open.
bool dispatch(RenderingDevice *rd, const RID &pipeline, std::initializer_list<SetSlot> sets,
		const PackedByteArray &push, uint32_t x, uint32_t y, uint32_t z = 1);

inline uint32_t groups(int n, int local) {
	return static_cast<uint32_t>((n + local - 1) / local);
}

// Writes command-record time (not GPU time) to `out_ms` when it leaves scope.
class CpuTimer {
public:
	explicit CpuTimer(float &out_ms) : out_(out_ms), t0_(std::chrono::steady_clock::now()) {}
	~CpuTimer() {
		out_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0_).count();
	}

private:
	float &out_;
	std::chrono::steady_clock::time_point t0_;
};

} // namespace godot::gpu
