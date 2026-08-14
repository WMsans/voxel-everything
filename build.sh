#!/usr/bin/env bash
#
# build.sh — build the Voxel Everything GDExtension native library.
#
# The GDScript errors like
#     Line 3: Could not find type "VoxelWorld" in the current scope.
# appear because Godot cannot load the compiled extension (extension/bin/
# libvoxel_everything.*.so is missing), so the native classes VoxelWorld /
# RaymarchCompositor are never registered. This script produces that library.
#
# Usage:
#   ./build.sh                build the shared library (linux debug template)
#   ./build.sh --test         also build & run the native C++ test suite
#   ./build.sh --verify       also load the project in headless Godot and check
#                             that the extension classes are found (no
#                             "Could not find type" parse errors)
#   ./build.sh --clean        remove build artifacts before building
#   ./build.sh -j N           override job count (default: nproc)
#
# Exit code 0 on success, non-zero on any failure.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXT_DIR="$ROOT/extension"
JOBS="$(nproc 2>/dev/null || echo 4)"
RUN_TESTS=0
RUN_VERIFY=0
CLEAN=0

i=0
args=("$@")
while [ $i -lt ${#args[@]} ]; do
	arg="${args[$i]}"
	case "$arg" in
		--test) RUN_TESTS=1 ;;
		--verify) RUN_VERIFY=1 ;;
		--clean) CLEAN=1 ;;
		-j) i=$((i + 1)); JOBS="${args[$i]:-}"; if [ -z "$JOBS" ]; then echo "build.sh: -j needs a number" >&2; exit 2; fi ;;
		-j*) JOBS="${arg#-j}" ;;
		-h|--help)
			sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*) echo "build.sh: unknown argument: $arg (see --help)" >&2; exit 2 ;;
	esac
	i=$((i + 1))
done

# --- prerequisites ---------------------------------------------------------
missing=()
for tool in git scons; do
	command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
# any C++ compiler (godot-cpp picks its own from the environment)
if ! command -v g++ >/dev/null 2>&1 && ! command -v clang++ >/dev/null 2>&1; then
	missing+=("a C++ compiler (g++ or clang++)")
fi
if [ "${#missing[@]}" -gt 0 ]; then
	echo "build.sh: missing required tools: ${missing[*]}" >&2
	exit 1
fi

# --- submodule -------------------------------------------------------------
# godot-cpp is a git submodule; fetch it if it has not been checked out.
if [ ! -f "$EXT_DIR/godot-cpp/SConstruct" ] || [ ! -e "$EXT_DIR/godot-cpp/.git" ]; then
	echo "==> Initializing godot-cpp submodule..."
	git -C "$ROOT" submodule update --init --recursive extension/godot-cpp
fi

# --- build -----------------------------------------------------------------
cd "$EXT_DIR"

if [ "$CLEAN" -eq 1 ]; then
	echo "==> Cleaning previous build artifacts..."
	scons -c -Q >/dev/null 2>&1 || true
	rm -rf bin build
fi

echo "==> Building libvoxel_everything with scons (-j$JOBS)..."
scons -j"$JOBS" -Q

# --- report ----------------------------------------------------------------
SO_DEBUG="$EXT_DIR/bin/libvoxel_everything.linux.template_debug.x86_64.so"
if [ ! -f "$SO_DEBUG" ]; then
	echo "build.sh: build finished but $SO_DEBUG was not produced" >&2
	exit 1
fi
echo
echo "==> Build OK: $(ls -lh "$SO_DEBUG" | awk '{print $5}') $(basename "$SO_DEBUG")"
echo "    Registered native classes: VoxelWorld, RaymarchCompositor"
echo "    Open the project in Godot (or press the reload button in the"
echo "    GDExtension inspector) — the 'Could not find type VoxelWorld'"
echo "    parse errors will disappear once the library is loaded."

# --- native test suite -----------------------------------------------------
if [ "$RUN_TESTS" -eq 1 ]; then
	echo
	echo "==> Building and running native test suite..."
	scons -j"$JOBS" -Q test
fi

# --- headless Godot check ---------------------------------------------------
if [ "$RUN_VERIFY" -eq 1 ]; then
	if ! command -v godot >/dev/null 2>&1; then
		echo "build.sh: --verify needs the 'godot' binary on PATH; skipping" >&2
		exit 1
	fi
	echo
	echo "==> Verifying extension loads and native types resolve (headless)..."
	# Redirect XDG dirs into the workspace: the DSH file sandbox (and CI-like
	# environments) may deny writes to ~/.local/share/godot, which makes Godot
	# crash on startup. In-tree redirects also keep the repo self-contained.
	mkdir -p "$ROOT/.xdgdata" "$ROOT/.xdgcache" "$ROOT/.xdgconfig"
	# Use editor mode: the project has no run/main_scene set, so plain `--quit`
	# aborts with "no main scene defined". Headless editor mode scans every
	# script — exactly where the 'Could not find type' errors surface.
	set +e
	out="$(XDG_DATA_HOME="$ROOT/.xdgdata" XDG_CACHE_HOME="$ROOT/.xdgcache" \
		XDG_CONFIG_HOME="$ROOT/.xdgconfig" \
		timeout 240 godot --headless --editor --quit --path "$ROOT" 2>&1)"
	status=$?
	set -e
	if [ $status -ne 0 ]; then
		echo "$out"
		echo "build.sh: godot exited with status $status" >&2
		exit 1
	fi
	if printf '%s\n' "$out" | grep -E 'Could not find type|Cannot infer the type' >/dev/null; then
		printf '%s\n' "$out" | grep -E 'Could not find type|Cannot infer the type' >&2
		echo "build.sh: native types still unresolved — see errors above" >&2
		exit 1
	fi
	echo "    OK: no 'Could not find type' parse errors reported by headless Godot."
fi

echo
echo "==> Done."
