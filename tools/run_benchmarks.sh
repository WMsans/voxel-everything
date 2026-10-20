#!/usr/bin/env bash
# Runs every benchmark leg and files the output. One label per run:
#   tools/run_benchmarks.sh m7-baseline
# Called with no arguments on a terminal, shows a menu instead and files into
# reports/${BENCH_LABEL:-interactive}/.
# Legs are sequential on purpose — two Godot processes sharing one GPU measure each other.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LABEL="${1:-${BENCH_LABEL:-interactive}}"
[ $# -gt 0 ] && shift
OUT="$ROOT/reports/$LABEL"

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

# Leg name -> godot args (word-split by the callers, so no spaces inside one); also the
# menu order.
NAMES=(steady move ridge edit edit-bounded island fjords-forest fjords-forest-walk)
leg_args() {
	case "$1" in
	steady) echo "--benchmark" ;;
	# Standing in the Fjords conifer forest: trunks, cards and grass fill the screen. Was 51 ms
	# on the Mac before bark took the R8 normal and the tree stages left the gradient taps.
	fjords-forest) echo "--benchmark --pipeline=res://assets/pipelines/fjords.pipeline" \
		"--seed=880510467 --pose=-98.05,186.0,417.40,59.0,-5" ;;
	# The same forest, walked through at ground level: 60 m at player.gd's 6 m/s along the
	# pose's yaw (59 deg), sampled at a fixed 1/60 s step so it covers the same 60 m whatever
	# the frame rate. This is the leg the forest caching work is judged on.
	fjords-forest-walk) echo "--benchmark-walk --pipeline=res://assets/pipelines/fjords.pipeline" \
		"--seed=880510467 --pose=-98.05,186.0,417.40,59.0,-5 --path-fps=60 --frames=600" ;;
	*) echo "--benchmark-$1" ;;
	esac
}

# $1 = name used for the report file, rest = godot args (the leg's, then any extras).
run_leg() {
	local name="$1"
	shift
	mkdir -p "$OUT"
	echo "=== $name ==="
	# Extra args belong AFTER the "--" separator: demo/benchmark.gd reads
	# OS.get_cmdline_user_args(), and Godot silently drops an unknown dashed argument
	# placed before the scene — a "--effects-off=..." run there measured every effect
	# still enabled.
	# ${a[@]+...} guards the empty-array case: macOS ships bash 3.2, where a bare
	# "${a[@]}" on an empty array trips `set -u`.
	"$GODOT" --path "$ROOT" ${DRIVER_ARGS[@]+"${DRIVER_ARGS[@]}"} --resolution 2560x1440 \
		--disable-vsync demo/scenes/main.tscn -- "$@" 2>&1 | tee "$OUT/$name.txt"
	echo "EXIT_STATUS=${PIPESTATUS[0]}" | tee -a "$OUT/$name.txt"
}

print_verdicts() {
	echo
	echo "=== verdicts ($LABEL) ==="
	grep -h "BENCH mode\|BENCH frame_avg_ms\|BENCH p50\|BENCH budget_verdict\|BENCH timing_condition" "$@" || true
}

if [ $# -eq 0 ] && [ -t 0 ] && [ -t 1 ] && command -v fzf >/dev/null 2>&1; then
	while :; do
		choice=$(printf '%s\n' all "${NAMES[@]}" |
			fzf --height 40% --reverse --no-sort --prompt "benchmark ($LABEL)> " \
				--header "enter: run   esc: quit") || break
		[ -n "$choice" ] || break
		if [ "$choice" = all ]; then
			for name in "${NAMES[@]}"; do
				run_leg "$name" $(leg_args "$name")
			done
			print_verdicts "$OUT"/*.txt
		else
			run_leg "$choice" $(leg_args "$choice")
			print_verdicts "$OUT/$choice.txt"
		fi
	done
	echo "=== reports in $OUT ==="
	exit 0
fi

for name in "${NAMES[@]}"; do
	run_leg "$name" $(leg_args "$name") "$@"
done
print_verdicts "$OUT"/*.txt
