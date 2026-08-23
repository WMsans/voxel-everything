#include "core/world_store.h"

namespace godot {

WorldStore::WorldStore(const ve::WorldConfig &config) : config_(config) {}

WorldStore::~WorldStore() {
	release_cores();
}

} // namespace godot
