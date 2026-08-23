#pragma once
// VoxelContext — the only thing subsystems see of each other (spec §4).
// Populated incrementally by Phases 2-5; subsystems receive the collaborators
// they need via constructor injection and never hold a VoxelWorld*.
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
