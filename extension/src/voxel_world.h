#pragma once
#include <godot_cpp/classes/node3d.hpp>

namespace godot {

class VoxelWorld : public Node3D {
	GDCLASS(VoxelWorld, Node3D)

protected:
	static void _bind_methods();

public:
	void _ready() override;
};

} // namespace godot
