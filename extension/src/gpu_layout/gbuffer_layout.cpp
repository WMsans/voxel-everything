#include "gpu_layout/gbuffer_layout.h"

namespace ve::layout {

std::string gbuffer_glsl() {
	std::string out =
			"// GENERATED from extension/src/gpu_layout/gbuffer_layout.h by ve::layout::gbuffer_glsl().\n"
			"// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
			"// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n"
			"//\n"
			"// G-buffer colour attachments in framebuffer order; depth follows them.\n";
	for (int i = 0; i < kGbColorAttachments; i++)
		out += "//   " + std::to_string(i) + " " + kGbAttachments[i].name + " " +
				kGbAttachments[i].format + ": " + kGbAttachments[i].channels + "\n";
	out += "// Macros only. GB_NORMAL and GB_PACK_SURFACE need common.glslh's oct_decode and\n"
	       "// oct_encode in scope where they are used.\n\n";
	out += "#define GB_ATTACHMENTS " + std::to_string(kGbColorAttachments) + "\n\n";
	for (const GbMacro &m : kGbMacros)
		out += std::string("#define ") + m.signature + " " + m.body + "\n";
	return out;
}

} // namespace ve::layout
