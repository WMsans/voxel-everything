#pragma once
// The G-buffer's colour attachments and how shaders read and write their channels. Generates
// shaders/generated/gbuffer.glslh. The file is macros only, so a shader may take it right after
// #version; GB_NORMAL and GB_PACK_SURFACE expand to common.glslh's oct_decode / oct_encode at
// the use site. render/gbuffer.cpp allocates the attachments and static_asserts the count.
#include <string>

namespace ve::layout {

struct GbAttachment {
	const char *name;
	const char *format;
	const char *channels;
};

inline constexpr GbAttachment kGbAttachments[] = {
	{"albedo", "R8G8B8A8_UNORM", "rgb = albedo, a = sun visibility"},
	{"surface", "R16G16B16A16_SFLOAT", "xy = oct normal, z = material id, w = gloss"},
};

inline constexpr int kGbColorAttachments =
		static_cast<int>(sizeof(kGbAttachments) / sizeof(kGbAttachments[0]));

struct GbMacro {
	const char *signature;
	const char *body;
};

inline constexpr GbMacro kGbMacros[] = {
	{"GB_ALBEDO(g0)", "((g0).rgb)"},
	{"GB_SUN_VIS(g0)", "((g0).a)"},
	{"GB_NORMAL(g1)", "oct_decode((g1).xy)"},
	{"GB_MATERIAL_ID(g1)", "uint((g1).z + 0.5)"},
	{"GB_IS_SURFACE(g1)", "((g1).z >= 0.5)"},
	{"GB_GLOSS(g1)", "((g1).w)"},
	{"GB_PACK_ALBEDO(rgb, sun_vis)", "vec4((rgb), (sun_vis))"},
	{"GB_PACK_SURFACE_OCT(oct, mat, gloss)", "vec4((oct), float(mat), (gloss))"},
	{"GB_PACK_SURFACE(n, mat, gloss)", "GB_PACK_SURFACE_OCT(oct_encode(n), (mat), (gloss))"},
};

// The exact contents of shaders/generated/gbuffer.glslh.
std::string gbuffer_glsl();

} // namespace ve::layout
