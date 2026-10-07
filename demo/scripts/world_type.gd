class_name WorldType
extends Resource
# One entry on the Create New World screen's World Type button. A world type IS a terrain
# pipeline (assets/pipelines/*.pipeline); this only gives it a name a player can read.
# Add a type by adding a .tres under demo/world_types/ -- no code changes.

@export var display_name := ""
@export_file("*.pipeline") var pipeline_path := ""
