extends Node
# One-shot: once the world can answer a ground query, put the player `lift_m` above the ground
# under it, then leave. Only the Create New World flow adds this node, so main.tscn run
# directly keeps its fixed (8, 62, 8) spawn for the benchmark and the captures.
# The player starts in FLY mode (player.gd), so it hovers there until the player presses F.

@export var world_path := ^"../VoxelWorld"
@export var player_path := ^"../Player"
@export var lift_m := 2.0
@export var timeout_s := 10.0

var _elapsed := 0.0

func _process(delta: float) -> void:
	var world := get_node_or_null(world_path)
	var player := get_node_or_null(player_path) as Node3D
	if world == null or player == null:
		queue_free()
		return
	var hit: Variant = world.ground_at(player.global_position.x, player.global_position.z)
	if hit != null:
		player.global_position = (hit as Vector3) + Vector3.UP * lift_m
		queue_free()
		return
	_elapsed += delta
	if _elapsed >= timeout_s:
		push_warning("GroundSpawn: the world did not initialise within %.0f s; " % timeout_s
				+ "leaving the player at the scene's spawn")
		queue_free()
