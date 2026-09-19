#!/usr/bin/env bash
# Converts the owner's bark_willow_1k PBR set into the 512^2 layer 07 (bark) PNGs under
# assets/materials/, so the build never depends on a path outside the repo. Run once; the
# outputs are committed. This is a sibling of convert_materials.sh, which only knows the
# terrain_textures_vol2 pack; that pack has no bark folder, so layer 07 comes from here.
# Same conventions as convert_materials.sh: 512x512! resize, -strip, PNG24, NN_<map>.png.
# Bark ships no 07_glow.png; MaterialAtlas then packs a flat 1.0 mask for it.
#
# Layer index 07 IS the texture-array layer order and must match ve::kMaterials in
# extension/src/world/material_table.h (the authoritative table): layer i serves ve material
# id i + 1. The converter agreement test in extension/tests/test_material_table.cpp only
# audits convert_materials.sh's MATERIALS list, so this separate script does not disturb it.
set -euo pipefail

SRC="${1:-/Users/jeremyzhao/Development/Unity/RayTraceVoxel/Assets/Textures/bark_willow_1k}"
DST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/materials"

NN=07

# map name -> source file, normal uses nor_gl (OpenGL convention, matching existing layers).
MAPS=(basecolor normal roughness ambientOcclusion height)
SOURCES=(bark_willow_diff_1k.jpg bark_willow_nor_gl_1k.png bark_willow_rough_1k.png \
	bark_willow_ao_1k.jpg bark_willow_disp_1k.png)

command -v convert >/dev/null || { echo "need ImageMagick 'convert'" >&2; exit 1; }
[ -d "$SRC" ] || { echo "source not found: $SRC" >&2; exit 1; }
mkdir -p "$DST"

for i in "${!MAPS[@]}"; do
	in="$SRC/${SOURCES[$i]}"
	out="$DST/${NN}_${MAPS[$i]}.png"
	if [ ! -f "$in" ]; then
		echo "missing $in" >&2
		exit 1
	fi
	convert "$in" -resize 512x512! -strip "PNG24:$out"
	echo "  $out"
done
echo "wrote layer $NN (bark) to $DST"
