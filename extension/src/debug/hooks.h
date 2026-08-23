#pragma once
#include <godot_cpp/classes/object.hpp>
// VoxelDebugHooks — all debug_*/test-fixture entry points extracted from
// VoxelWorld (spec Phase 1). Registered as a Godot class so GDScript tests
// call world.hooks().debug_x(...). Holds no state of its own beyond a
// back-reference set by its owning VoxelWorld.
namespace godot {
class VoxelWorld;
class VoxelDebugHooks : public Object {
    GDCLASS(VoxelDebugHooks, Object)
public:
    void bind_world(VoxelWorld *w) { world_ = w; }
protected:
    static void _bind_methods();
private:
    VoxelWorld *world_ = nullptr;
};
} // namespace godot
