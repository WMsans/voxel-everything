#include "render/lod_cull_pass.h"
#include "lod/lod_contour.h"
#include "render/hiz_pass.h"
#include "render/lod_pool.h"
#include "gpu_layout/blocks.h"
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <algorithm>
#include <chrono>

using namespace godot;

LodCullPass::~LodCullPass() {
	teardown();
}

bool LodCullPass::initialize(RenderingDevice *rd) {
	teardown();
	if (!rd) return false;
	rd_ = rd;

	program_ = gpu::compile_compute(rd, group_, "LodCullPass", "lod_cull.comp.glsl");
	if (!program_.valid()) {
		teardown();
		return false;
	}
	sampler_ = gpu::sampler(rd, group_, RenderingDevice::SAMPLER_FILTER_NEAREST);
	if (!sampler_.is_valid()) {
		teardown();
		return false;
	}

	PackedByteArray zero;
	zero.resize(4);
	zero.fill(0);
	stats_ = group_.add(gpu::Kind::Buffer, rd->storage_buffer_create(4, zero));
	if (!stats_.is_valid()) {
		teardown();
		return false;
	}

	stats_readback_.instantiate();
	if (stats_readback_.is_null()) {
		teardown();
		return false;
	}
	args_readback_.instantiate();
	if (args_readback_.is_null()) {
		teardown();
		return false;
	}
	return true;
}

void LodCullPass::teardown() {
	if (!rd_) return;
	// Complete pending staging-buffer callbacks while their source buffer and RefCounted
	// targets are still alive; RenderingDevice has no cancellation operation.
	if (stats_readback_.is_valid()) stats_readback_->drain(rd_);
	if (args_readback_.is_valid()) args_readback_->drain(rd_);
	gpu::RdDevice device{rd_};
	group_.release(device);
	program_ = gpu::Program();
	sampler_ = stats_ = RID();
	set_ = gpu::SetCache();
	stats_readback_ = Ref<AsyncBufferRead>();
	args_readback_ = Ref<AsyncBufferRead>();
	first_pass_pages_.clear();
	first_pass_pages_at_request_.clear();
	last_visible_pages_.clear();
	last_drawn_ = 0;
	last_total_ = 0;
	last_total_at_request_ = 0;
	last_first_pass_count_at_request_ = 0;
	last_remaining_count_at_request_ = 0;
	rd_ = nullptr;
}

bool LodCullPass::ensure_uniform_set(RenderingDevice *rd, LodPool &pool, HizPass *hiz) {
	if (!hiz || !hiz->pyramid().is_valid()) return false;
	gpu::RdDevice device{rd};
	return set_.get(device, group_, program_.shader, 0, {
			gpu::storage(0, pool.args_buffer()),
			gpu::storage(1, pool.page_chunk_buffer()),
			gpu::storage(2, pool.chunk_buffer()),
			gpu::sampled(3, sampler_, hiz->pyramid()),
			gpu::storage(4, stats_)}).is_valid();
}

void LodCullPass::consume_args_readback() {
	const PackedByteArray &data = args_readback_->data();
	const int remaining_count = last_remaining_count_at_request_;
	if (remaining_count <= 0 || data.size() < static_cast<int64_t>(remaining_count) * 20) {
		return; // fail-soft: keep the previous visible set
	}

	std::vector<int> visible = first_pass_pages_at_request_;
	const uint32_t *a = reinterpret_cast<const uint32_t *>(data.ptr());
	for (int i = 0; i < remaining_count; i++) {
		if (a[static_cast<size_t>(i) * 5u + 1u] != 0u) {
			const uint32_t page = a[static_cast<size_t>(i) * 5u + 3u] /
					static_cast<uint32_t>(ve::kLodVertsPerPage);
			visible.push_back(static_cast<int>(page));
		}
	}
	std::sort(visible.begin(), visible.end());
	visible.erase(std::unique(visible.begin(), visible.end()), visible.end());
	last_visible_pages_.swap(visible);
}

void LodCullPass::set_last_visible_pages(const std::vector<int> &pages) {
	last_visible_pages_ = pages;
	std::sort(last_visible_pages_.begin(), last_visible_pages_.end());
	last_visible_pages_.erase(
			std::unique(last_visible_pages_.begin(), last_visible_pages_.end()),
			last_visible_pages_.end());
	// A manual visible-set update supersedes any in-flight args readback. Invalidate the
	// request-time pairing so consume_args_readback() ignores a stale readback that arrives
	// later and would otherwise re-promote pages that left draw_pages().
	last_remaining_count_at_request_ = 0;
	first_pass_pages_at_request_.clear();
}

bool LodCullPass::run(RenderingDevice *rd, LodPool &pool, HizPass *hiz,
		const Projection &view_proj, int page_count, int total_page_count,
		int first_pass_count) {
	const auto t0 = std::chrono::steady_clock::now();
	if (!rd || !rd_ || !program_.valid() || !stats_readback_.is_valid() ||
			!args_readback_.is_valid()) {
		return false;
	}
	if (page_count <= 0) return false;
	if (!ensure_uniform_set(rd, pool, hiz)) return false;

	if (stats_readback_->take_fresh()) {
		last_drawn_ = stats_readback_->as_i32();
		last_total_ = last_total_at_request_;
		if (last_total_ > 0) last_drawn_ += last_first_pass_count_at_request_;
	}
	if (args_readback_->take_fresh()) consume_args_readback();

	// Stats clear must be recorded before the compute list opens (device-level command).
	PackedByteArray zero;
	zero.resize(4);
	zero.fill(0);
	rd->buffer_update(stats_, 0, 4, zero);

	const int64_t list = rd->compute_list_begin();
	rd->compute_list_bind_compute_pipeline(list, program_.pipeline);
	rd->compute_list_bind_uniform_set(list, set_.id(), 0);

	// The shader derives frustum planes from view_proj.
	ve::LodCullPush push{};
	for (int c = 0; c < 4; c++)
		for (int r = 0; r < 4; r++)
			push.view_proj[c * 4 + r] = view_proj.columns[c][r]; // GLSL mat4 = column-major
	push.params[0] = page_count;
	push.params[1] = HizPass::kSize;
	push.params[2] = hiz->mip_count();

	rd->compute_list_set_push_constant(list, gpu::push_bytes(push), sizeof(push));
	rd->compute_list_dispatch(list, (static_cast<uint32_t>(page_count) + 63u) / 64u, 1, 1);
	rd->compute_list_end();

	// Async stats readback for the HUD's culled ratio. Never synchronous. Record the whole
	// candidate total and the first-pass count paired with the request so a later readback
	// is divided by the right denominator.
	if (stats_readback_->request(rd, stats_, 0, 4)) {
		last_total_at_request_ = total_page_count;
		last_first_pass_count_at_request_ = first_pass_count;
	}
	// Async args readback: the CPU learns which remaining pages survived the cull a few
	// frames later. Paired with the first-pass page snapshot from the same request so
	// last_visible_pages() is the exact union of that frame's two passes. A zero
	// first-pass count (debug probe / single-pass cull) must snapshot an empty first-pass
	// list so stale first-pass pages from a normal temporal frame are not folded in.
	if (args_readback_->request(rd, pool.args_buffer(), 0,
				static_cast<uint32_t>(page_count) * 20u)) {
		if (first_pass_count <= 0) {
			first_pass_pages_at_request_.clear();
		} else {
			first_pass_pages_at_request_ = first_pass_pages_;
		}
		last_remaining_count_at_request_ = page_count;
	}
	last_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
	return true;
}
