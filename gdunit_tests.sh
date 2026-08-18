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
# Usage:
#   ./gdunit_tests.sh                       run every test in res://tests
#   ./gdunit_tests.sh -a res://tests/test_gpu_atlas.gd,res://tests/test_gpu_smoke.gd
#                                             run only the given suite(s)
#   ./gdunit_tests.sh -a res://tests/a.gd -a res://tests/b.gd
#                                             same thing, written out
#   ./gdunit_tests.sh --godot_binary /path/to/godot
#   GODOT_BIN=/path/to/godot ./gdunit_tests.sh
#
# Any other gdUnit4 CLI args (-i, -c, -rd, -conf, ...) are passed through.
#
# Exit code is the gdUnit4 test run's exit code, except that discovering no
# test cases at all is reported as a failure (gdUnit4 itself exits 0 there).
#
# --- why this invokes Godot directly ---------------------------------------
# It used to shell out to addons/gdUnit4/runtest.sh. That wrapper does three
# things this script needs to control:
#   * it runs `godot --path .`, i.e. relative to the CALLER's directory, so the
#     script only worked when invoked from the repo root;
#   * it passes `-d --remote-debug tcp://127.0.0.1:0` to keep Godot's
#     interactive `debug>` prompt from hanging the run on a parse error. Port 0
#     is never valid, so every single run printed two spurious ERROR lines. Not
#     passing `-d` at all achieves the same thing without the noise: a parse
#     error then exits with gdUnit4's own RETURN_ERROR_SCRIPT (105);
#   * it launches a second, headless Godot afterwards to run GdUnitCopyLog.gd,
#     which folds Godot's log file into the HTML report. This project does not
#     enable `debug/file_logging/enable_file_logging`, so that pass only ever
#     wrote a "No logging available!" placeholder — a whole extra engine start
#     per run for nothing.
# Driving Godot from here also lets engine flags be placed BEFORE the script
# path, which is where Godot documents them.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

godot_binary="${GODOT_BIN:-}"
args=("$@")
test_paths=()
passthrough=()

i=0
while [ $i -lt ${#args[@]} ]; do
	arg="${args[$i]}"
	case "$arg" in
		--godot_binary)
			if [ $((i + 1)) -ge ${#args[@]} ]; then
				echo "gdunit_tests.sh: --godot_binary needs a path" >&2
				exit 2
			fi
			i=$((i + 1))
			godot_binary="${args[$i]}"
			;;
		-a)
			# A missing value here used to be swallowed silently, which quietly
			# widened `-a` into "run every suite in res://tests" — minutes of GPU
			# tests instead of the one that was asked for.
			if [ $((i + 1)) -ge ${#args[@]} ]; then
				echo "gdunit_tests.sh: -a needs a test path (or a comma-separated list)" >&2
				exit 2
			fi
			i=$((i + 1))
			# gdUnit4's -a takes ONE path and may be repeated; it does NOT split on
			# commas. This script has always documented the comma-separated form, so it
			# was passing the whole list as a single path -- gdUnit4 then reported
			# "Given directory or file does not exists", ran nothing, and (see below)
			# exited 0. Split here so both spellings work.
			while IFS= read -r one; do
				if [ -n "$one" ]; then
					test_paths+=("$one")
				fi
			done <<< "${args[$i]//,/$'\n'}"
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
if [ ! -x "$godot_binary" ]; then
	echo "gdunit_tests.sh: '$godot_binary' is not an executable Godot binary." >&2
	exit 1
fi

if [ ${#test_paths[@]} -eq 0 ]; then
	test_paths=("res://tests")
fi

# --- make sure a real display is reachable ---------------------------------
# This script is often invoked from a shell (SSH/tmux) that has no DISPLAY
# or WAYLAND_DISPLAY exported even though the desktop session is running.
# Without one, Godot falls back to the headless DisplayServer regardless of
# flags, and gdUnit4 aborts. Auto-detect the running session's sockets.
if [ -z "${WAYLAND_DISPLAY:-}" ] && [ -z "${DISPLAY:-}" ]; then
	runtime_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
	# Sorted, so a session with several sockets always picks the same one
	# instead of whatever order the directory happens to be in.
	wayland_sock="$(find "$runtime_dir" -maxdepth 1 -name 'wayland-*' ! -name '*.lock' 2>/dev/null | sort | head -1)"
	if [ -n "$wayland_sock" ]; then
		export XDG_RUNTIME_DIR="$runtime_dir"
		WAYLAND_DISPLAY="$(basename "$wayland_sock")"
		export WAYLAND_DISPLAY
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

# --- vsync ------------------------------------------------------------------
# MEASURED, not assumed: with vsync on, this suite's window is normally
# unfocused and occluded, so the compositor throttles its presents and a single
# `await get_tree().process_frame` costs ~590 ms instead of ~0.5 ms. Tests that
# step the world frame by frame (the LoD suites step it hundreds of times) then
# spend all their wall clock in the swapchain: test_lod_pool.gd took 8m36s, of
# which the LoD work itself was under a second. Presentation timing changes
# nothing any test asserts on, so the runner always turns it off.
godot_flags=(--path "$ROOT" --disable-vsync)

echo "==> Running gdUnit4 against a real display (GPU rendering enabled, vsync off)"
echo "    godot_binary: $godot_binary"
echo "    tests:        ${test_paths[*]}"

suite_args=()
for one in "${test_paths[@]}"; do
	suite_args+=(-a "$one")
done

cd "$ROOT"
log="$(mktemp -t gdunit_tests.XXXXXX)"
trap 'rm -f "$log"' EXIT

status=0
"$godot_binary" "${godot_flags[@]}" -s res://addons/gdUnit4/bin/GdUnitCmdTool.gd \
	"${suite_args[@]}" ${passthrough[@]+"${passthrough[@]}"} 2>&1 | tee "$log"
status=${PIPESTATUS[0]}

# GdUnitTestCIRunner.run() quits with RETURN_SUCCESS when discovery turns up nothing, so a
# typo in a suite path used to produce a green run that tested exactly zero things. A test
# runner that reports success without running anything is worse than one that fails.
if [ "$status" -eq 0 ] && grep -q "No test cases found" "$log"; then
	echo "gdunit_tests.sh: gdUnit4 discovered no test cases under: ${test_paths[*]}" >&2
	echo "  It exits 0 in that case; this script does not." >&2
	status=1
fi

echo "Run tests ends with $status"
exit "$status"
