#include "gpu_layout/layout.h"

namespace ve::layout {

namespace {

size_t bytes(FieldType type) {
	return type == FieldType::Mat4 ? 64 : 16;
}

const char *glsl_type(FieldType type) {
	switch (type) {
		case FieldType::Vec4: return "vec4";
		case FieldType::IVec4: return "ivec4";
		case FieldType::UVec4: return "uvec4";
		case FieldType::Mat4: return "mat4";
	}
	return "vec4";
}

} // namespace

std::string check_block(const Block &block) {
	size_t at = 0;
	for (int i = 0; i < block.field_count; i++) {
		const Field &f = block.fields[i];
		if (f.offset != at)
			return std::string(block.macro) + ": " + f.name + " is at offset " +
					std::to_string(f.offset) + " in C++ but at " + std::to_string(at) + " in GLSL";
		at += bytes(f.type) * static_cast<size_t>(f.count > 0 ? f.count : 1);
	}
	if (at != block.size)
		return std::string(block.macro) + ": the fields cover " + std::to_string(at) +
				" bytes but sizeof is " + std::to_string(block.size);
	return "";
}

std::string emit_block(const Block &block) {
	std::string out = std::string("#define ") + block.macro + " \\\n";
	for (int i = 0; i < block.field_count; i++) {
		const Field &f = block.fields[i];
		out += std::string("\t") + glsl_type(f.type) + " " + f.name;
		if (f.count > 0) out += "[" + std::to_string(f.count) + "]";
		out += i + 1 < block.field_count ? "; \\\n" : ";\n";
	}
	return out;
}

} // namespace ve::layout
