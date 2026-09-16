#pragma once
// Lifetime rules every GPU pass shares, with no godot-cpp dependency so the native suite can
// drive them against a fake device (extension/tests/test_gpu_core.cpp). The RenderingDevice
// binding is render/gpu/gpu.h.
//
// A Device supplies:
//   using Id = ...;                  // copyable; == and !=; Id{} means "none"
//   bool alive(Kind, const Id &);    // false once freed, directly or by cascade
//   void free(const Id &);
//   Id create_uniform_set(const Id &shader, uint32_t set, const std::vector<Uniform<Id>> &);
//
// RenderingDevice frees dependents with their dependency: a uniform set dies with its shader
// and with any texture, buffer or sampler it binds; a pipeline with its shader; a framebuffer
// with any attachment. Freeing an id that already died is an error. So every free below asks
// alive() first, and release() frees dependents before what they reference.
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace ve::gpu {

// Values are RenderingDevice::UniformType's; render/gpu/gpu.cpp static_asserts each.
enum class UniformType : uint8_t {
	SamplerWithTexture = 1,
	Image = 3,
	UniformBuffer = 7,
	StorageBuffer = 8,
};

// Declaration order is free order: nothing here references a kind declared after it.
enum class Kind : uint8_t { UniformSet, IndexArray, Framebuffer, Pipeline, Shader, Sampler, Texture, Buffer };
inline constexpr int kKindCount = 8;

template <class Id>
struct Uniform {
	UniformType type = UniformType::Image;
	uint32_t binding = 0;
	Id ids[2] = {};
	uint8_t count = 0;
	// Explicit rather than `= default`: the main build uses C++20, and a defaulted comparison
	// operator is C++20 (gcc rejects it, clang merely warns).
	bool operator==(const Uniform &o) const {
		return type == o.type && binding == o.binding && count == o.count && ids[0] == o.ids[0] && ids[1] == o.ids[1];
	}
};

template <class Id>
Uniform<Id> sampled(uint32_t binding, const Id &sampler, const Id &texture) {
	return {UniformType::SamplerWithTexture, binding, {sampler, texture}, 2};
}

template <class Id>
Uniform<Id> image(uint32_t binding, const Id &texture) {
	return {UniformType::Image, binding, {texture, Id{}}, 1};
}

template <class Id>
Uniform<Id> ubo(uint32_t binding, const Id &buffer) {
	return {UniformType::UniformBuffer, binding, {buffer, Id{}}, 1};
}

template <class Id>
Uniform<Id> storage(uint32_t binding, const Id &buffer) {
	return {UniformType::StorageBuffer, binding, {buffer, Id{}}, 1};
}

// Everything one pass owns on one device. A pass registers each RID as it creates it; its
// teardown is one release().
template <class Device>
class ResourceGroup {
public:
	using Id = typename Device::Id;

	Id add(Kind kind, const Id &id) {
		if (id != Id{}) entries_.push_back({kind, id});
		return id;
	}

	// Frees one owned id now (if the device still has it) and forgets it. Ids the group does
	// not own are ignored.
	void free(Device &device, const Id &id) {
		for (size_t i = entries_.size(); i-- > 0;) {
			if (entries_[i].id != id) continue;
			if (device.alive(entries_[i].kind, id)) device.free(id);
			entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
			return;
		}
	}

	// Kind by kind in declaration order; within a kind, newest first, so a texture view
	// registered after its parent goes before it.
	void release(Device &device) {
		for (int k = 0; k < kKindCount; k++)
			for (size_t i = entries_.size(); i-- > 0;) {
				const Entry &e = entries_[i];
				if (static_cast<int>(e.kind) == k && device.alive(e.kind, e.id)) device.free(e.id);
			}
		entries_.clear();
	}

	size_t size() const { return entries_.size(); }

private:
	struct Entry {
		Kind kind;
		Id id;
	};
	std::vector<Entry> entries_;
};

// One uniform set whose inputs can change. get() returns the cached set while the shader, the
// set index and every bound id match and the device still has it; otherwise it frees the old
// set (if alive) and builds a new one. Godot never reuses an RID, so a recreated texture is a
// changed key by itself.
template <class Device>
class UniformSetCache {
public:
	using Id = typename Device::Id;

	Id get(Device &device, ResourceGroup<Device> &group, const Id &shader, uint32_t set,
			std::vector<Uniform<Id>> uniforms) {
		if (set_ != Id{} && shader == shader_ && set == index_ && uniforms == key_ &&
				device.alive(Kind::UniformSet, set_))
			return set_;
		drop(device, group);
		set_ = group.add(Kind::UniformSet, device.create_uniform_set(shader, set, uniforms));
		if (set_ != Id{}) {
			shader_ = shader;
			index_ = set;
			key_ = std::move(uniforms);
		}
		return set_;
	}

	void drop(Device &device, ResourceGroup<Device> &group) {
		if (set_ != Id{}) group.free(device, set_);
		set_ = Id{};
		shader_ = Id{};
		index_ = 0;
		key_.clear();
	}

	const Id &id() const { return set_; }

private:
	Id set_{};
	Id shader_{};
	uint32_t index_ = 0;
	std::vector<Uniform<Id>> key_;
};

// A set whose inputs never change identity: built once, released with the group.
template <class Device>
typename Device::Id uniform_set(Device &device, ResourceGroup<Device> &group,
		const typename Device::Id &shader, uint32_t set,
		const std::vector<Uniform<typename Device::Id>> &uniforms) {
	return group.add(Kind::UniformSet, device.create_uniform_set(shader, set, uniforms));
}

} // namespace ve::gpu
