// Unit test for ve::pack_field_params_bytes: the dense-in-112 packing shared by
// FieldContextSet::initialize and VoxelDebugHooks::debug_field_params_bytes.
#include <doctest/doctest.h>
#include <cstring>
#include "terrain/field_params_pack.h"

namespace {

ve::ResolvedPipeline pipeline_with(const std::vector<float> &values) {
	ve::ResolvedPipeline p;
	for (float v : values) {
		ve::ParamDecl d;
		d.value = v;
		p.params.push_back(d);
	}
	return p;
}

float read_float(const std::vector<uint8_t> &bytes, size_t offset) {
	float v = 0.0f;
	std::memcpy(&v, bytes.data() + offset, 4);
	return v;
}

} // namespace

TEST_CASE("pack_field_params_bytes: 7-param dense layout") {
	ve::ResolvedPipeline p = pipeline_with({1.0f, 2.5f, -3.0f, 0.0f, 6.0f, 3.0f, 1.0f});
	const std::vector<uint8_t> bytes = ve::pack_field_params_bytes(p);
	CHECK(bytes.size() == 112);
	for (size_t i = 0; i < p.params.size(); i++)
		CHECK(read_float(bytes, i * 4) == doctest::Approx(p.params[i].value));
}

TEST_CASE("pack_field_params_bytes: empty pipeline yields 16 zero bytes") {
	ve::ResolvedPipeline p;
	const std::vector<uint8_t> bytes = ve::pack_field_params_bytes(p);
	CHECK(bytes.size() == 16);
	for (uint8_t b : bytes) CHECK(b == 0);
}
