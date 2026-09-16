#include "gpu_layout/constants.h"
#include "generator/volume_set.h"
#include "shade/beauty_settings.h"
#include "world/brick.h"
#include "world/material_table.h"
#include <sstream>

namespace ve::layout {

std::string constants_glsl() {
	std::ostringstream o;
	o << "// GENERATED from C++ constants by ve::layout::constants_glsl() (extension/src/gpu_layout/).\n"
	     "// Do not edit by hand: extension/tests/test_generated_glsl.cpp asserts this file byte\n"
	     "// for byte. Regenerate: cd extension && VE_REGEN_GOLDEN=1 ./build/tests/ve_tests\n"
	     "\n"
	     "// ve::kBeautyFlags (extension/src/shade/beauty_settings.h)\n";
	for (const BeautyFlag &f : kBeautyFlags)
		o << "const uint BEAUTY_" << f.name << " = " << f.bit << "u;\n";
	o << "\n"
	     "// ve::kMaterialLayers (extension/src/world/material_table.h)\n"
	     "const int MATERIAL_LAYERS = " << kMaterialLayers << ";\n"
	     "\n"
	     "// Brick, volume and override-pool strides (world/brick.h, generator/volume_set.h)\n"
	     "const int BRICK_VOXEL_COUNT = " << kBrickVoxelCount << ";\n"
	     "const int BRICK_SDF_COUNT = " << kBrickSdfCount << ";\n"
	     "const int VOLUME_VOXELS = " << kIslandVoxelCount << ";\n"
	     "const int OVERRIDE_SDF_STRIDE_BYTES = " << kOverrideSdfStrideBytes << ";\n"
	     "const int OVERRIDE_MAT_STRIDE_BYTES = " << kOverrideMatStrideBytes << ";\n";
	return o.str();
}

} // namespace ve::layout
