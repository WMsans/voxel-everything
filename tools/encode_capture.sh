#!/usr/bin/env bash
# Turns the PNG sequence demo/capture.gd writes into an H.264 file at a true 60 fps.
#   tools/encode_capture.sh ~/.local/share/godot/app_userdata/<project>/capture out.mp4
set -euo pipefail
DIR="${1:?usage: encode_capture.sh <png-dir> [out.mp4]}"
OUT="${2:-voxel-everything.mp4}"
ffmpeg -y -framerate 60 -i "$DIR/frame_%05d.png" \
	-c:v libx264 -preset slow -crf 16 -pix_fmt yuv420p "$OUT"
echo "wrote $OUT"
