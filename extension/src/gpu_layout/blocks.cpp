#include "gpu_layout/blocks.h"

namespace ve::layout {

std::string blocks_glsl() {
	std::string out =
			"// GENERATED from extension/src/gpu_layout/blocks.h by ve::layout::blocks_glsl().\n"
			"// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
			"// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n"
			"//\n"
			"// Each macro is the field list of one push-constant or uniform block. The shader keeps\n"
			"// layout(), set, binding and instance name around it; the C++ struct carries the\n"
			"// comments on what each field holds.\n";
	for (const Block &b : kBlocks) out += "\n" + emit_block(b);
	return out;
}

} // namespace ve::layout
