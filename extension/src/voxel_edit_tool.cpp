#include "voxel_edit_tool.h"
#include "voxel_world.h"
#include "world/material_table.h"
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cmath>

using namespace godot;

void VoxelEditTool::_bind_methods() {
	// `material` defaults to 0 (air), whose fail-soft hardness is 1.0, so every existing
	// caller that passes only a position and a radius keeps carving at full size.
	ClassDB::bind_method(D_METHOD("apply_sphere_subtract", "pos", "radius", "material"),
			&VoxelEditTool::apply_sphere_subtract, DEFVAL(0));
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
	// Hostile-input guard: NaN/Inf positions or radius, or a non-positive / absurd
	// radius, would corrupt op_region_range (int cast of NaN is UB) and could freeze
	// the edit log for minutes. Refuse without touching the log.
	if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z) ||
			!std::isfinite(radius) || radius <= 0.0f || radius > 1000.0f) {
		UtilityFunctions::printerr("VoxelEditTool: invalid op rejected (pos ", pos.x, ", ",
				pos.y, ", ", pos.z, ", radius ", radius, ")");
		return out;
	}
	ve::EditOp op{};
	op.type = type;
	// Material is stored as a 16-bit id in the op and sampler; clamp hostile values.
	op.material = static_cast<uint32_t>(std::clamp(material, 0, 65535));
	op.pos[0] = pos.x; op.pos[1] = pos.y; op.pos[2] = pos.z;
	// Resistance is resolved HERE, once, from the material the centre ray struck -- never
	// per sample inside the field. `radius` is the tool's nominal reach; what the op stores
	// is its exact geometric reach, which is what op_world_aabb and every consumer built on
	// it (region ranges, brick residency, the island blast impulse) then agree with.
	// A future non-spherical removal scales its own dimensions the same way.
	op.radius = type == ve::kOpSphereSubtract
			? ve::removal_radius(radius, static_cast<uint16_t>(op.material))
			: radius;
	const ve::EditLog::AppendResult r = world->append_edit(op);
	for (const ve::IVec3 &v : r.touched) touched.push_back(Vector3i(v.x, v.y, v.z));
	for (const ve::IVec3 &v : r.rejected) rejected.push_back(Vector3i(v.x, v.y, v.z));
	return out;
}

Dictionary VoxelEditTool::apply_sphere_subtract(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSphereSubtract, pos, radius, material);
}

Dictionary VoxelEditTool::apply_sphere_add(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSphereAdd, pos, radius, material);
}

Dictionary VoxelEditTool::apply_sphere_paint(Vector3 pos, float radius, int material) {
	return apply(ve::kOpSpherePaint, pos, radius, material);
}
