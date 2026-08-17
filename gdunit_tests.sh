#!/usr/bin/env bash
#
# gdunit_tests.sh — run the gdUnit4 suite with a real GPU-backed rendering
# device instead of headless mode.
#
# Most of this project's tests exercise VoxelWorld/RaymarchCompositor, which
# are GPU-driven (compute dispatch, atlas textures, viewport screenshots).
# gdUnit4 itself refuses to run at all when Godot's DisplayServer is
# "headless" (see GdUnitTestCIRunner.init_gd_unit): it exits with
# RETURN_ERROR_HEADLESS_NOT_SUPPORTED unless --ignoreHeadlessMode is passed,
# and even then, viewport-screenshot / rendering-device tests would just see
# a dummy driver instead of the real GPU. So this script does NOT pass
# --headless — it launches Godot against a real X11/Wayland display so
# DisplayServer picks a real driver (vulkan) backed by the actual GPU.
#
# It reuses addons/gdUnit4/runtest.sh, which already omits --headless from
# the underlying Godot invocation.
#
# Usage:
#   ./gdunit_tests.sh                       run every test in res://tests
#   ./gdunit_tests.sh -a res://tests/test_gpu_atlas.gd,res://tests/test_gpu_smoke.gd
#                                             run only the given suite(s)
#   ./gdunit_tests.sh --godot_binary /path/to/godot
#   GODOT_BIN=/path/to/godot ./gdunit_tests.sh
#
# Any other gdUnit4 CLI args (-i, -c, -rd, -conf, ...) are passed through.
#
# Exit code is the gdUnit4 test run's exit code.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

godot_binary="${GODOT_BIN:-}"
args=("$@")
test_paths=""
passthrough=()

i=0
while [ $i -lt ${#args[@]} ]; do
	arg="${args[$i]}"
	case "$arg" in
		--godot_binary)
			i=$((i + 1))
			godot_binary="${args[$i]:-}"
			;;
		-a)
			i=$((i + 1))
			test_paths="${args[$i]:-}"
			;;
		*)
			passthrough+=("$arg")
			;;
	esac
	i=$((i + 1))
done

if [ -z "$godot_binary" ]; then
	godot_binary="$(command -v godot || true)"
fi
if [ -z "$godot_binary" ]; then
	echo "gdunit_tests.sh: no Godot binary found. Set GODOT_BIN or pass --godot_binary." >&2
	exit 1
fi

if [ -z "$test_paths" ]; then
	test_paths="res://tests"
fi

# --- make sure a real display is reachable ---------------------------------
# This script is often invoked from a shell (SSH/tmux) that has no DISPLAY
# or WAYLAND_DISPLAY exported even though the desktop session is running.
# Without one, Godot falls back to the headless DisplayServer regardless of
# flags, and gdUnit4 aborts. Auto-detect the running session's sockets.
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
	runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
	wayland_sock="$(find "$runtime_dir" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' 2>/dev/null | head -1)"
	if [ -n "$wayland_sock" ]; then
		export XDG_RUNTIME_DIR="$runtime_dir"
		export WAYLAND_DISPLAY="$(basename "$wayland_sock")"
		echo "==> No display exported; using detected Wayland socket: $WAYLAND_DISPLAY"
	elif [ -S /tmp/.X11-unix/X0 ]; then
		export DISPLAY=":0"
		echo "==> No display exported; using detected X11 display: $DISPLAY"
	else
		echo "gdunit_tests.sh: no X11 or Wayland display found. GPU tests need a real" >&2
		echo "  desktop session (headless mode is rejected by gdUnit4)." >&2
		exit 1
	fi
fi

echo "==> Running gdUnit4 against a real display (GPU rendering enabled)"
echo "    godot_binary: $godot_binary"
echo "    tests:        $test_paths"

exec bash "$ROOT/addons/gdUnit4/runtest.sh" --godot_binary "$godot_binary" -a "$test_paths" "${passthrough[@]}"
