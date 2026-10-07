extends CharacterBody3D
# Demo character (spec section 6: "Character = standard CharacterBody3D capsule").
#
# Two modes, F toggles: FLY (no gravity, no collision, the M2 fly camera's controls) and
# WALK (gravity, move_and_slide against the streamed colliders). It starts in FLY on purpose
# — the collider streamer needs a second or so to build the first chunks, and a walking body
# dropped into a world whose colliders do not exist yet would fall straight through.
#
# P pauses the game. This node keeps PROCESS_MODE_ALWAYS so P can also unpause, but physics
# and gameplay input are skipped while paused; the HUD/help overlays stay readable.

@export var walk_speed := 6.0
@export var fly_speed := 25.0
@export var jump_velocity := 5.5
@export var look_sensitivity := 0.0025
@export var gravity := 24.0

var flying := true

# Radial impulse from the edit tool's explosion kick. It is added to velocity once,
# immediately before move_and_slide(), so walking input/gravity cannot overwrite it.
var _impulse := Vector3.ZERO

@onready var _cam: Camera3D = $Camera3D

func _ready() -> void:
	process_mode = Node.PROCESS_MODE_ALWAYS
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_P:
		get_tree().paused = not get_tree().paused
		return
	if get_tree().paused:
		# While paused, only allow releasing/capturing the mouse so the player can reach
		# the window controls or unpause with P; no gameplay input.
		if event.is_action_pressed("ui_cancel"):
			Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED
		return
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-event.relative.x * look_sensitivity)
		_cam.rotate_x(-event.relative.y * look_sensitivity)
		_cam.rotation.x = clampf(_cam.rotation.x, -1.45, 1.45)
	if event.is_action_pressed("ui_cancel"):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE if Input.mouse_mode == Input.MOUSE_MODE_CAPTURED else Input.MOUSE_MODE_CAPTURED
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_F:
		flying = not flying
		velocity = Vector3.ZERO

func _physics_process(delta: float) -> void:
	if get_tree().paused:
		return
	var dir := Vector3.ZERO
	# Flying steers with the camera (pitch included); walking steers with the body.
	var basis := _cam.global_transform.basis if flying else global_transform.basis
	if Input.is_key_pressed(KEY_W): dir -= basis.z
	if Input.is_key_pressed(KEY_S): dir += basis.z
	if Input.is_key_pressed(KEY_A): dir -= basis.x
	if Input.is_key_pressed(KEY_D): dir += basis.x

	if flying:
		var lift := 0.0
		if Input.is_key_pressed(KEY_E): lift += 1.0
		if Input.is_key_pressed(KEY_Q): lift -= 1.0
		var boost := 4.0 if Input.is_key_pressed(KEY_SHIFT) else 1.0
		velocity = (dir.normalized() + Vector3.UP * lift) * fly_speed * boost
		_impulse = Vector3.ZERO # kicks only apply in walk mode; don't carry a stale one
		global_position += velocity * delta # no collision in fly mode
		return

	dir.y = 0.0
	dir = dir.normalized()
	velocity.x = dir.x * walk_speed
	velocity.z = dir.z * walk_speed
	if is_on_floor():
		velocity.y = jump_velocity if Input.is_key_pressed(KEY_SPACE) else 0.0
	else:
		velocity.y -= gravity * delta
	# Apply the pending kick after all normal velocity computation, so the impulse cannot
	# be overwritten by WASD, floor snapping, or gravity before move_and_slide() sees it.
	velocity += _impulse
	_impulse = Vector3.ZERO
	move_and_slide()
