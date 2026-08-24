#include "world/material_table.h"

namespace ve {

float material_hardness(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 1.0f;
	return kMaterials[i].hardness;
}

float material_glow(uint16_t id) {
	const int i = static_cast<int>(id) - 1;
	if (i < 0 || i >= kMaterialCount) return 0.0f;
	return kMaterials[i].glow;
}

} // namespace ve
