#include "render/async_readback.h"
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable_method_pointer.hpp>

using namespace godot;

void AsyncBufferRead::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_data", "data"), &AsyncBufferRead::_on_data);
}

void AsyncBufferRead::_on_data(const PackedByteArray &data) {
	data_ = data;
	pending_ = false;
	has_data_ = true;
	fresh_ = true;
}

bool AsyncBufferRead::request(RenderingDevice *rd, const RID &buffer, uint32_t offset,
		uint32_t size) {
	if (pending_ || !rd || !buffer.is_valid()) return false;
	// Recorded at this point in the command stream, so anything the caller records AFTER it
	// (a clear of the word just read, say) cannot race the copy.
	if (rd->buffer_get_data_async(buffer, callable_mp(this, &AsyncBufferRead::_on_data), offset,
				size) != OK)
		return false;
	pending_ = true;
	return true;
}

int32_t AsyncBufferRead::as_i32(int64_t index) const {
	const int64_t off = index * 4;
	if (data_.size() < off + 4) return 0;
	return *reinterpret_cast<const int32_t *>(data_.ptr() + off);
}

void AsyncTextureRead::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_data", "data"), &AsyncTextureRead::_on_data);
}

void AsyncTextureRead::_on_data(const PackedByteArray &data) {
	data_ = data;
	pending_ = false;
	has_data_ = true;
	fresh_ = true;
}

bool AsyncTextureRead::request(RenderingDevice *rd, const RID &texture) {
	if (pending_ || !rd || !texture.is_valid()) return false;
	if (rd->texture_get_data_async(texture, 0, callable_mp(this, &AsyncTextureRead::_on_data)) != OK)
		return false;
	pending_ = true;
	return true;
}
