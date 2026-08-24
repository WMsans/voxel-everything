#!/usr/bin/env bash
# Runs every benchmark leg and files the output. One label per run:
#   tools/run_benchmarks.sh m7-baseline
# Legs are sequential on purpose — two Godot processes sharing one GPU measure each other.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:?usage: run_benchmarks.sh <label> [extra godot args...]}"
shift || true
OUT="$ROOT/reports/$LABEL"
mkdir -p "$OUT"

GODOT="${GODOT_BIN:-godot}"
command -v "$GODOT" >/dev/null 2>&1 || GODOT=/usr/bin/godot

# Display-server selection is a Linux concern: x11 honours --disable-vsync, the Wayland
# backend on that machine does not (M6 errata 2), and XWayland satisfies it under a Wayland
# session -- so prefer x11 there and fall back loudly. macOS has one backend, it honours
# --disable-vsync (verified: `vsync_actual=disabled`), and passing --display-driver at all
# makes Godot reject the run.
DRIVER_ARGS=()
case "$(uname -s)" in
Darwin) ;;
*)
	DRIVER="x11"
	if [ -z "${DISPLAY:-}" ]; then
		DRIVER="wayland"
		echo "WARNING: no DISPLAY; running on the Wayland backend. Frame percentiles will be" \
			"V-Sync-capped and every frame verdict is qualified." >&2
	fi
	DRIVER_ARGS=(--display-driver "$DRIVER")
	;;
esac

LEGS=(--benchmark --benchmark-move --benchmark-ridge --benchmark-edit --benchmark-edit-bounded --benchmark-island)
for leg in "${LEGS[@]}"; do
	name="${leg#--benchmark}"
	name="${name#-}"
	[ -z "$name" ] && name="steady"
	echo "=== $name ==="
	# Extra args belong AFTER the "--" separator: demo/benchmark.gd reads
	# OS.get_cmdline_user_args(), and Godot silently drops an unknown dashed argument
	# placed before the scene — a "--effects-off=..." run there measured every effect
	# still enabled.
	# ${a[@]+...} guards the empty-array case: macOS ships bash 3.2, where a bare
	# "${a[@]}" on an empty array trips `set -u`.
	"$GODOT" --path "$ROOT" ${DRIVER_ARGS[@]+"${DRIVER_ARGS[@]}"} --resolution 2560x1440 \
		--disable-vsync demo/main.tscn -- "$leg" "$@" 2>&1 | tee "$OUT/$name.txt"
	echo "EXIT_STATUS=${PIPESTATUS[0]}" | tee -a "$OUT/$name.txt"
done

echo
echo "=== verdicts ($LABEL) ==="
grep -h "BENCH mode\|BENCH frame_avg_ms\|BENCH p50\|BENCH budget_verdict\|BENCH timing_condition" "$OUT"/*.txt
