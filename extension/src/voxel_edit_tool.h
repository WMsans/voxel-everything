#pragma once
#include <godot_cpp/classes/node.hpp>

namespace godot {

class VoxelWorld;

// Thin Godot-facing op emitter (spec §5: the demo tools are "just op emitters"). All the
// real work lives in VoxelWorld::append_edit and the streamer; this class only packs the
// ve::EditOp and reports which regions accepted it.
class VoxelEditTool : public Node {
	GDCLASS(VoxelEditTool, Node)

protected:
	static void _bind_methods();

public:
	// `material` is the material the centre ray struck; its hardness shrinks the removal
	// once, at construction. Omitting it means material 0, whose hardness is 1.0.
	Dictionary apply_sphere_subtract(Vector3 pos, float radius, int material = 0);
	Dictionary apply_sphere_add(Vector3 pos, float radius, int material);
	Dictionary apply_sphere_paint(Vector3 pos, float radius, int material);

private:
	Dictionary apply(uint32_t type, Vector3 pos, float radius, int material);
};

} // namespace godot
