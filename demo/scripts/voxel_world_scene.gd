class_name VoxelWorldScene
extends VoxelWorld
# The reusable world (spec §4): terrain, settings, environment, compositors and sun in one
# scene that works under any parent. Instance res://demo/scenes/voxel_world.tscn, set
# physics_center_path to your player, and pick a world_type / world_seed BEFORE it enters
# the tree -- the terrain pipeline loads once, at first init.

# High enough to clear every shipped terrain: SURFACE_Y 51.2 + relief 310 + hills 10.
const GROUND_PROBE_Y := 600.0

@export var world_type: WorldType:
	set(value):
		world_type = value
		if value != null:
			terrain_pipeline_path = value.pipeline_path

func _enter_tree() -> void:
	# The compositor effects find their world by an ABSOLUTE path from the scene root, so
	# they can only be pointed here once this node has one. Each instance gets its own
	# copies: a scene's sub-resources are shared between instances, and two worlds writing
	# one world_path would both render the last one.
	var we := get_node_or_null("WorldEnvironment") as WorldEnvironment
	if we == null or we.compositor == null:
		return
	var effects: Array[CompositorEffect] = []
	for effect in we.compositor.compositor_effects:
		var copy := effect.duplicate() as CompositorEffect
		if "world_path" in copy:
			copy.set("world_path", get_path())
		effects.append(copy)
	var compositor := Compositor.new()
	compositor.compositor_effects = effects
	we.compositor = compositor

# The terrain surface straight below (x, GROUND_PROBE_Y, z), or null until the pipeline has
# loaded. raycast() queries the CPU field analytically, so it answers for ground that has
# not streamed yet.
func ground_at(x: float, z: float) -> Variant:
	if not is_initialized():
		return null
	var hit: Dictionary = raycast(Vector3(x, GROUND_PROBE_Y, z), Vector3.DOWN, GROUND_PROBE_Y * 2.0)
	return hit["pos"] if hit["hit"] else null
