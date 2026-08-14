#include "voxel_edit_tool.h"
#include "voxel_world.h"
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VoxelEditTool::_bind_methods() {
	ClassDB::bind_method(D_METHOD("apply_sphere_subtract", "pos", "radius"),
			&VoxelEditTool::apply_sphere_subtract);
	ClassDB::bind_method(D_METHOD("apply_sphere_add", "pos", "radius", "material"),
			&VoxelEditTool::apply_sphere_add);
	ClassDB::bind_method(D_METHOD("apply_sphere_paint", "pos", "radius", "material"),
			&VoxelEditTool::apply_sphere_paint);
}

Dictionary VoxelEditTool::apply(uint32_t type, Vector3 pos, float radius, int material) {
	Dictionary out;
	Array touched, rejected;
	out["touched"] = touched;
	out["rejected"] = rejected;
	VoxelWorld *world = Object::cast_to<VoxelWorld>(get_parent());
	if (!world) {
		UtilityFunctions::printerr("VoxelEditTool: parent is not a VoxelWorld");
		return out;
	}
	ve::EditOp op{};
	op.type = type;
	op.material = static_cast<uint32_t>(material);
	op.pos[0] = pos.x; op.pos[1] = pos.y; op.pos[2] = pos.z;
	op.radius = radius;
	const ve::EditLog::AppendResult r = world->append_edit(op);
	for (const ve::IVec3 &v : r.touched) touched.push_back(Vector3i(v.x, v.y, v.z));
	for (const ve::IVec3 &v : r.rejected) rejected.push_back(Vector3i(v.x, v.y, v.z));
	return out;
}

Dictionary VoxelEditTool::apply_sphere_subtract(Vector3 pos, float radius) {
	return apply(ve::kOpSphereSubtract, pos, radius, 0);
}

Dictionary VoxelEditTool::apply_sphere_add(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSphereAdd, pos, radius, material);
}

Dictionary VoxelEditTool::apply_sphere_paint(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSpherePaint, pos, radius, material);
}
