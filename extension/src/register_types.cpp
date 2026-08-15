#include "register_types.h"
#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include "voxel_world.h"
#include "voxel_edit_tool.h"
#include "raymarch_compositor.h"
#include "render/async_readback.h"

using namespace godot;

void voxel_everything_initialize(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
	// Internal: it exists only so callable_mp can name its handler for the async readbacks.
	GDREGISTER_INTERNAL_CLASS(AsyncBufferRead);
	GDREGISTER_CLASS(VoxelWorld);
	GDREGISTER_CLASS(RaymarchCompositor);
	GDREGISTER_CLASS(VoxelEditTool);
}

void voxel_everything_uninitialize(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
}

extern "C" {
GDExtensionBool GDE_EXPORT voxel_everything_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		const GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
	init_obj.register_initializer(voxel_everything_initialize);
	init_obj.register_terminator(voxel_everything_uninitialize);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_obj.init();
}
}
