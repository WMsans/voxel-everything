#pragma once
// VoxelContext — VoxelWorld's wiring record: the pointers handed to each subsystem at
// construction. Subsystems do NOT see each other through this struct; each one receives
// its own Collaborators struct via constructor injection and never holds a VoxelWorld*.
#include "world/region.h"

namespace godot {
class WorldStore;
class RenderOrchestrator;
class LodSystem;
class ConsolidationCoordinator;

struct VoxelContext {
    WorldStore *store = nullptr;
    RenderOrchestrator *render = nullptr;
    LodSystem *lod = nullptr;
    ConsolidationCoordinator *consolidation = nullptr;
};
} // namespace godot
