#pragma once
// Describes a push-constant or uniform block once, in C++, and writes its GLSL field list.
// Only 16-byte-aligned GLSL types are allowed (vec4, ivec4, uvec4, mat4 and arrays of them):
// for those, std140 and std430 put every field at the same offset, so one rule covers push
// constants and uniform buffers and check_block can prove a table against offsetof.
#include <cstddef>
#include <string>

namespace ve::layout {

enum class FieldType { Vec4, IVec4, UVec4, Mat4 };

struct Field {
	const char *name;
	FieldType type;
	int count;     // 0 = one value; N = an array of N
	size_t offset; // offsetof in the C++ struct
};

struct Block {
	const char *macro; // GLSL field-list macro, e.g. "SSAO_PUSH_FIELDS"
	size_t size;       // sizeof the C++ struct
	const Field *fields;
	int field_count;
};

// "" when each field starts where the previous one ends and the fields cover `size` exactly;
// otherwise one sentence naming the block and the first disagreement.
std::string check_block(const Block &block);

// "#define <macro> \\\n\t<type> <name>[N]; \\\n ... \t<type> <name>;\n"
std::string emit_block(const Block &block);

} // namespace ve::layout

#define VE_LAYOUT_FIELD(S, member, type, count) \
	::ve::layout::Field{#member, ::ve::layout::FieldType::type, count, offsetof(S, member)}

#define VE_LAYOUT_BLOCK(S, macro, table) \
	::ve::layout::Block{macro, sizeof(S), table, static_cast<int>(sizeof(table) / sizeof(table[0]))}
