#!/usr/bin/env bash
# Converts the subset of terrain_textures_vol2 the demo uses into 512^2 PNGs under
# assets/materials/, so the build never depends on a path outside the repo. Run once; the
# outputs are committed. 512^2 is set by memory: 1.4 MB per layer per array with mips, so 16
# materials is ~45 MB, and 1024^2 would be 4x that against the 0.7-1.0 GB brick atlas.
set -euo pipefail

SRC="${1:-/home/jeremy/Development/Unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2}"
DST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/materials"

# Index order IS the texture-array layer order and must match ve::kMaterials in
# extension/src/world/material_table.h (the authoritative table). Layer i serves ve material
# id i + 1; material 0 is air and has no layer. Agreement is enforced by the converter
# test in extension/tests/test_material_table.cpp.
MATERIALS=(grass_01 rock ground_01 breakstone)
MAPS=(basecolor normal roughness ambientOcclusion height)

command -v convert >/dev/null || { echo "need ImageMagick 'convert'" >&2; exit 1; }
[ -d "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }
mkdir -p "$DST"

for i in "${!MATERIALS[@]}"; do
	m="${MATERIALS[$i]}"
	for map in "${MAPS[@]}"; do
		in="$SRC/$m/T_${m}_${map}.tga"
		out="$DST/$(printf '%02d' "$i")_${map}.png"
		if [ ! -f "$in" ]; then
			echo "missing $in" >&2
			exit 1
		fi
		convert "$in" -resize 512x512! -strip "PNG24:$out"
		echo "  $out"
	done
done
echo "wrote ${#MATERIALS[@]} materials to $DST"
