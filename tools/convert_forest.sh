#!/usr/bin/env bash
# Builds layer 10 (forest) from terrain_textures_vol2's ground_foliage_01, recoloured toward
# dark needle green. Up close it reads as needle litter between the conifer cards; from far
# away the far field sees only the top mip, which is the canopy colour the references show
# (docs/superpowers/specs/2026-10-08-fjords-conifers-design.md §8). A sibling of
# convert_bark.sh: convert_materials.sh lists `forest` in MATERIALS (the table test reads that
# line) and skips it in its loop, because the pack has no forest folder.
#
# The recolour is the tuning knob: change MODULATE / TINT and re-run, no code changes.
# Same conventions as convert_materials.sh: 512x512! resize, -strip, PNG24, NN_<map>.png.
set -euo pipefail

SRC="${1:-/Users/jeremyzhao/Development/unity/RayTraceVoxel/Assets/Textures/terrain_textures_vol2}"
DST="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/assets/materials"
NN=10
M=ground_foliage_01
# brightness,saturation,hue (ImageMagick -modulate): darker, a little more saturated, hue
# rotated from olive toward blue-green.
MODULATE="55,115,112"
# A multiply toward the reference canopy colour after the modulate.
TINT="rgb(150,190,165)"

command -v magick >/dev/null || { echo "need ImageMagick 'magick'" >&2; exit 1; }
[ -d "$SRC/$M" ] || { echo "source not found: $SRC/$M" >&2; exit 1; }
mkdir -p "$DST"

magick "$SRC/$M/T_${M}_basecolor.tga" -resize 512x512! -modulate "$MODULATE" \
	\( +clone -fill "$TINT" -colorize 100 \) -compose multiply -composite \
	-strip "PNG24:$DST/${NN}_basecolor.png"
echo "  $DST/${NN}_basecolor.png"
for map in normal roughness ambientOcclusion height; do
	magick "$SRC/$M/T_${M}_${map}.tga" -resize 512x512! -strip "PNG24:$DST/${NN}_${map}.png"
	echo "  $DST/${NN}_${map}.png"
done
echo "wrote layer $NN (forest) to $DST"
