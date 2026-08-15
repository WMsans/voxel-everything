#pragma once
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/rendering_device.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

namespace godot {

// One outstanding RenderingDevice::buffer_get_data_async, plus the last bytes it returned.
//
// It exists because buffer_get_data "will block the GPU from working until the data is
// retrieved" (the engine's own note), and the streamer read three counters that way on every
// frame. That turned each frame into CPU-then-GPU instead of CPU-alongside-GPU: measured at
// 1.6-3.6 ms of render-thread stall on average and 39.6 ms on the worst frame of an editing
// run, because the read waits for whatever generation work the previous frame queued.
//
// The asynchronous form costs nothing on the frame that asks; the bytes turn up a few frames
// later (ProjectSettings rendering/rendering_device/vsync/frame_queue_size). Callers must
// therefore treat what they read as a few frames stale — see WorldStreamer, which corrects
// the free-slot count by what it has spent since the request went out.
//
// Internal: registered only so callable_mp can name the handler. Nothing scripts this.
class AsyncBufferRead : public RefCounted {
	GDCLASS(AsyncBufferRead, RefCounted)

	PackedByteArray data_;
	bool pending_ = false;
	bool has_data_ = false;
	bool fresh_ = false;

protected:
	static void _bind_methods();

public:
	void _on_data(const PackedByteArray &data);

	// Issues a read unless one is already outstanding. Returns true if one went out.
	bool request(RenderingDevice *rd, const RID &buffer, uint32_t offset, uint32_t size);
	bool pending() const { return pending_; }
	bool has_data() const { return has_data_; }
	// True once per arrival, so a caller can rebase its bookkeeping exactly when the value
	// it is holding changes rather than every frame.
	bool take_fresh() {
		const bool f = fresh_;
		fresh_ = false;
		return f;
	}
	const PackedByteArray &data() const { return data_; }
	int32_t as_i32(int64_t index = 0) const;
};

} // namespace godot
