#include <doctest/doctest.h>
#include "render/gpu/gpu_core.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <vector>

using ve::gpu::Kind;
using U = ve::gpu::Uniform<uint64_t>;

namespace {

// The two RenderingDevice rules gpu_core.h is written against: freeing a resource kills
// everything that references it (a uniform set dies with its shader and with any id it binds;
// a pipeline with its shader; a framebuffer with its attachments), and freeing a dead id is an
// error -- counted here, so a test can assert it never happens.
struct FakeDevice {
	using Id = uint64_t;
	struct Res {
		Kind kind;
		std::vector<uint64_t> refs;
		bool alive = true;
	};
	std::map<uint64_t, Res> res;
	std::vector<uint64_t> freed;
	uint64_t next = 1;
	int bad_frees = 0;
	int sets_created = 0;

	uint64_t make(Kind kind, std::vector<uint64_t> refs = {}) {
		res[next] = Res{kind, std::move(refs)};
		return next++;
	}
	bool alive(Kind, const uint64_t &id) {
		const auto it = res.find(id);
		return it != res.end() && it->second.alive;
	}
	void kill(uint64_t id) {
		res[id].alive = false;
		for (auto &[other, r] : res)
			if (r.alive && std::find(r.refs.begin(), r.refs.end(), id) != r.refs.end()) kill(other);
	}
	void free(const uint64_t &id) {
		if (!alive(Kind::Buffer, id)) {
			bad_frees++;
			return;
		}
		freed.push_back(id);
		kill(id);
	}
	uint64_t create_uniform_set(const uint64_t &shader, uint32_t, const std::vector<U> &uniforms) {
		std::vector<uint64_t> refs{shader};
		for (const U &u : uniforms)
			for (uint8_t i = 0; i < u.count; i++) refs.push_back(u.ids[i]);
		for (uint64_t r : refs)
			if (!alive(Kind::Buffer, r)) return 0;
		sets_created++;
		return make(Kind::UniformSet, refs);
	}
	int live() const {
		int n = 0;
		for (const auto &[id, r] : res) n += r.alive ? 1 : 0;
		return n;
	}
};

using Group = ve::gpu::ResourceGroup<FakeDevice>;
using Cache = ve::gpu::UniformSetCache<FakeDevice>;

U img(uint32_t binding, uint64_t texture) { return ve::gpu::image<uint64_t>(binding, texture); }

} // namespace

TEST_CASE("a uniform set is reused while its shader, set index and every bound id match") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t a = c.get(d, g, shader, 0, {img(0, tex)});
	const uint64_t b = c.get(d, g, shader, 0, {img(0, tex)});
	CHECK(a != 0);
	CHECK(a == b);
	CHECK(c.id() == a);
	CHECK(d.sets_created == 1);
}

TEST_CASE("a changed id, binding, shader or set index rebuilds and frees the old set") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t s1 = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t s2 = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t t1 = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t t2 = g.add(Kind::Texture, d.make(Kind::Texture));
	uint64_t prev = c.get(d, g, s1, 0, {img(0, t1)});
	auto rebuilt = [&](uint64_t now) {
		CHECK(now != 0);
		CHECK(now != prev);
		CHECK_FALSE(d.alive(Kind::UniformSet, prev));
		prev = now;
	};
	rebuilt(c.get(d, g, s1, 0, {img(0, t2)}));
	rebuilt(c.get(d, g, s1, 0, {img(1, t2)}));
	rebuilt(c.get(d, g, s2, 0, {img(1, t2)}));
	rebuilt(c.get(d, g, s2, 2, {img(1, t2)}));
	CHECK(d.bad_frees == 0);
	CHECK(g.size() == 5); // two shaders, two textures, the one live set
}

TEST_CASE("a set its texture took down is rebuilt, and nothing is freed twice") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t first = c.get(d, g, shader, 0, {img(0, tex)});
	g.free(d, tex); // a resize: the old target goes and takes the set with it
	CHECK_FALSE(d.alive(Kind::UniformSet, first));
	tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t second = c.get(d, g, shader, 0, {img(0, tex)});
	CHECK(second != first);
	CHECK(d.alive(Kind::UniformSet, second));
	CHECK(d.bad_frees == 0);
}

TEST_CASE("a set that died while its key still matches is rebuilt, not reused") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t first = c.get(d, g, shader, 0, {img(0, tex)});
	d.kill(first); // the device dropped it for a reason this pass cannot see
	const uint64_t second = c.get(d, g, shader, 0, {img(0, tex)});
	CHECK(second != first);
	CHECK(d.alive(Kind::UniformSet, second));
	CHECK(d.bad_frees == 0);
}

TEST_CASE("a failed create returns none, keeps no key and retries on the next call") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t dead = d.make(Kind::Texture);
	d.free(dead);
	CHECK(c.get(d, g, shader, 0, {img(0, dead)}) == 0);
	CHECK(c.id() == 0);
	CHECK(g.size() == 1);
	const uint64_t live = g.add(Kind::Texture, d.make(Kind::Texture));
	CHECK(c.get(d, g, shader, 0, {img(0, live)}) != 0);
}

TEST_CASE("drop is idempotent, safe after a cascade and a no-op after release") {
	FakeDevice d;
	Group g;
	Cache c;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	c.get(d, g, shader, 0, {img(0, tex)});
	g.free(d, tex);
	c.drop(d, g);
	c.drop(d, g);
	CHECK(c.id() == 0);
	CHECK(d.bad_frees == 0);
	const uint64_t tex2 = g.add(Kind::Texture, d.make(Kind::Texture));
	c.get(d, g, shader, 0, {img(0, tex2)});
	g.release(d);
	c.drop(d, g);
	CHECK(d.bad_frees == 0);
}

TEST_CASE("release frees dependents first whatever the registration order, and leaves nothing") {
	FakeDevice d;
	Group g;
	Cache c;
	// Registered dependencies-first on purpose: release must not rely on registration order.
	const uint64_t tex = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t buf = g.add(Kind::Buffer, d.make(Kind::Buffer));
	const uint64_t smp = g.add(Kind::Sampler, d.make(Kind::Sampler));
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t pipeline = g.add(Kind::Pipeline, d.make(Kind::Pipeline, {shader}));
	const uint64_t fb = g.add(Kind::Framebuffer, d.make(Kind::Framebuffer, {tex}));
	const uint64_t set = c.get(d, g, shader, 0,
			{ve::gpu::sampled<uint64_t>(0, smp, tex), ve::gpu::storage<uint64_t>(1, buf)});
	g.release(d);
	CHECK(d.bad_frees == 0);
	CHECK(d.live() == 0);
	CHECK(g.size() == 0);
	CHECK(d.freed == std::vector<uint64_t>{set, fb, pipeline, shader, smp, tex, buf});
}

TEST_CASE("within one kind, release frees the newest first") {
	FakeDevice d;
	Group g;
	const uint64_t parent = g.add(Kind::Texture, d.make(Kind::Texture));
	const uint64_t view = g.add(Kind::Texture, d.make(Kind::Texture, {parent}));
	g.release(d);
	CHECK(d.freed == std::vector<uint64_t>{view, parent});
	CHECK(d.bad_frees == 0);
}

TEST_CASE("release skips what a cascade already took, and a group is reusable after it") {
	FakeDevice d;
	Group g;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	g.add(Kind::Pipeline, d.make(Kind::Pipeline, {shader}));
	d.free(shader); // the pipeline dies with it
	g.release(d);
	CHECK(d.bad_frees == 0);
	CHECK(g.size() == 0);
	const uint64_t again = g.add(Kind::Texture, d.make(Kind::Texture));
	g.release(d);
	CHECK_FALSE(d.alive(Kind::Texture, again));
	CHECK(d.live() == 0);
}

TEST_CASE("add ignores none, free ignores ids the group does not own") {
	FakeDevice d;
	Group g;
	CHECK(g.add(Kind::Texture, 0) == 0);
	CHECK(g.size() == 0);
	const uint64_t foreign = d.make(Kind::Buffer);
	g.free(d, foreign);
	CHECK(d.alive(Kind::Buffer, foreign));
	CHECK(d.bad_frees == 0);
}

TEST_CASE("uniform_set builds a set once and registers it for release") {
	FakeDevice d;
	Group g;
	const uint64_t shader = g.add(Kind::Shader, d.make(Kind::Shader));
	const uint64_t buf = g.add(Kind::Buffer, d.make(Kind::Buffer));
	const uint64_t set = ve::gpu::uniform_set(d, g, shader, 0, {ve::gpu::storage<uint64_t>(0, buf)});
	CHECK(set != 0);
	CHECK(g.size() == 3);
	g.release(d);
	CHECK(d.live() == 0);
}
