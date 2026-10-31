#!/usr/bin/env bash
# Generates synthetic test clips (no external media needed) into .testdata/
set -euo pipefail
OUT="${1:-$(dirname "$0")/../.testdata}"
SECS="${2:-20}"
NAME="${3:-test1080}"
mkdir -p "$OUT"

# ${SECS} s, 1080p30, moving ball + burned-in timestamp, 440 Hz tone
gst-launch-1.0 -q \
  videotestsrc num-buffers=$((SECS*30)) pattern=ball ! video/x-raw,width=1920,height=1080,framerate=30/1 \
    ! timeoverlay ! x264enc bitrate=6000 speed-preset=ultrafast ! h264parse ! mp4mux name=mux \
    ! filesink location="$OUT/${NAME}.mp4" \
  audiotestsrc num-buffers=$((SECS*47)) wave=sine freq=440 ! audio/x-raw,rate=48000,channels=2 \
    ! avenc_aac ! aacparse ! mux.

echo "wrote $OUT/${NAME}.mp4 (${SECS}s)"
ls -la "$OUT"
