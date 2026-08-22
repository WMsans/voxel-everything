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

# x11 honours --disable-vsync; the Wayland backend on this machine does not (M6 errata 2).
# XWayland satisfies it under a Wayland session, so prefer x11 and fall back loudly.
DRIVER="x11"
if [ -z "${DISPLAY:-}" ]; then
	DRIVER="wayland"
	echo "WARNING: no DISPLAY; running on the Wayland backend. Frame percentiles will be" \
		"V-Sync-capped and every frame verdict is qualified." >&2
fi

LEGS=(--benchmark --benchmark-move --benchmark-ridge --benchmark-edit --benchmark-edit-bounded --benchmark-island)
for leg in "${LEGS[@]}"; do
	name="${leg#--benchmark}"
	name="${name#-}"
	[ -z "$name" ] && name="steady"
	echo "=== $name ($DRIVER) ==="
	# Extra args belong AFTER the "--" separator: demo/benchmark.gd reads
	# OS.get_cmdline_user_args(), and Godot silently drops an unknown dashed argument
	# placed before the scene — a "--effects-off=..." run there measured every effect
	# still enabled.
	/usr/bin/godot --path "$ROOT" --display-driver "$DRIVER" --resolution 2560x1440 \
		--disable-vsync demo/main.tscn -- "$leg" "$@" 2>&1 | tee "$OUT/$name.txt"
	echo "EXIT_STATUS=${PIPESTATUS[0]}" | tee -a "$OUT/$name.txt"
done

echo
echo "=== verdicts ($LABEL) ==="
grep -h "BENCH budget_verdict\|BENCH timing_condition" "$OUT"/*.txt
