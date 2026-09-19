#include <doctest/doctest.h>
#include "gpu_layout/blocks.h"
#include "shade/beauty_settings.h"
#include <set>
#include <string>

using ve::layout::Block;
using ve::layout::Field;
using ve::layout::FieldType;

TEST_CASE("every block table matches its C++ struct") {
	for (const Block &b : ve::layout::kBlocks) {
		const std::string why = ve::layout::check_block(b);
		CHECK_MESSAGE(why.empty(), why);
	}
}

TEST_CASE("a table that misplaces or omits a field is refused") {
	const Field gap[] = {{"a", FieldType::Vec4, 0, 0}, {"b", FieldType::Vec4, 0, 20}};
	const std::string moved = ve::layout::check_block({"GAP", 36, gap, 2});
	CHECK(moved.find("GAP: b") != std::string::npos);
	const Field short_table[] = {{"a", FieldType::Vec4, 0, 0}};
	const std::string missing = ve::layout::check_block({"SHORT", 32, short_table, 1});
	CHECK(missing.find("cover 16 bytes") != std::string::npos);
	const Field arrays[] = {{"m", FieldType::Mat4, 3, 0}, {"v", FieldType::Vec4, 2, 192}};
	CHECK(ve::layout::check_block({"ARRAYS", 224, arrays, 2}).empty());
}

TEST_CASE("beauty flags are distinct single bits") {
	uint32_t seen = 0;
	for (const ve::BeautyFlag &f : ve::kBeautyFlags) {
		CHECK_MESSAGE(f.bit != 0, f.name);
		CHECK_MESSAGE((f.bit & (f.bit - 1)) == 0, f.name);
		CHECK_MESSAGE((seen & f.bit) == 0, f.name);
		seen |= f.bit;
	}
	CHECK(sizeof(ve::kBeautyFlags) / sizeof(ve::kBeautyFlags[0]) == 9);
}

TEST_CASE("block macros are unique and emit one declaration per field") {
	std::set<std::string> names;
	for (const Block &b : ve::layout::kBlocks) CHECK_MESSAGE(names.insert(b.macro).second, b.macro);
	CHECK(names.size() == 31);
	const Field f[] = {{"view_proj", FieldType::Mat4, 3, 0}, {"splits", FieldType::UVec4, 0, 192}};
	CHECK(ve::layout::emit_block({"X_FIELDS", 208, f, 2}) ==
			"#define X_FIELDS \\\n\tmat4 view_proj[3]; \\\n\tuvec4 splits;\n");
}
